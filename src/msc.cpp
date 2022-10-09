/** $lic$
 * Copyright (C) 2019-2022 by Daniel Sanchez
 *
 * This file is part of the Minispec compiler and toolset.
 *
 * Minispec is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, version 2.
 *
 * Minispec is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <algorithm>
#include <cctype>
#include <iostream>
#include <filesystem>
#include <regex>
#include <unordered_set>
#include <variant>
#include "antlr4-runtime.h"
#include "argparse/argparse.hpp"
#include "errors.h"
#include "log.h"
#include "parse.h"
#include "strutils.h"
#include "translate.h"
#include "version.h"
#include "MinispecLexer.h"
#include "MinispecParser.h"

using namespace antlr4;

// Bluespec errors
typedef std::function<std::string(uint32_t, uint32_t)> TranslateLocFn;
typedef std::function<std::string(uint32_t, uint32_t, const std::vector<std::string>&)> ContextStrFn;
typedef std::function<tree::ParseTree*(uint32_t, uint32_t)> FindFn;

void reportBluespecOutput(std::string str, const SourceMap& sm, const std::string& topLevel, bool simOut) {
    typedef std::regex_iterator<std::string::const_iterator> regex_it;
    const regex_it rend;

    // C++ regexes suck... I want ECMAScript mode with multiline (it seems
    // lookahead doesn't work with std::regex::extended), but
    // std::regex:multiline is a C++17 feature, and isn't even in gcc 8.2! So
    // substitute all newlines with a line termination token that doesn't show
    // up in Bluespec output.
    const char* lineTerm = " _@%@_ ";
    replace(str, "\n", lineTerm);
    std::regex evRegex("(Warning|Error): (.*?)(?=( _@%@_ Error:| _@%@_ Warning:|$))");

    const std::string locRegexStr = "\"(\\S+)\",\\s+line\\s+(\\d+),\\s+column\\s+(\\d+)";
    std::regex locRegex(locRegexStr);
    std::regex hdrRegex(locRegexStr + ":\\s+\\((\\S+)\\)"); // include type

    auto translateLoc = [&](uint32_t line, uint32_t lineChar) {
        auto pt = sm.find(line, lineChar);
        if (pt) return getLoc(pt);
        else return "(translated bsv:" + std::to_string(line) + ":" + std::to_string(lineChar) + ")";
    };

    auto translateAllLocs = [&](std::string& msg) {
        std::smatch locMatch;
        std::unordered_map<std::string, std::tuple<uint32_t, uint32_t>> locToPos;
        while (std::regex_search(msg, locMatch, locRegex)) {
            std::string file = locMatch[1];
            uint32_t line = atoi(locMatch[2].str().c_str());
            uint32_t lineChar = atoi(locMatch[3].str().c_str());
            std::string loc;
            if (file == "Translated.bsv") {
                loc = translateLoc(line, lineChar);
            } else {
                loc = file + ":" + std::to_string(line) + ":" + std::to_string(lineChar);
            }
            replace(msg, locMatch[0], hlColored(loc));
            locToPos[hlColored(loc)] = std::tie(line, lineChar);
        }
        return locToPos;
    };

    auto contextStrFn = [&](uint32_t line, uint32_t lineChar,
            const std::vector<std::string>& elems) -> std::string
    {
        tree::ParseTree* ctx = nullptr;
        for (auto elem : elems) {
            ctx = sm.find(line, lineChar, elem);
            if (ctx) break;
        }
        if (!ctx) ctx = sm.find(line, lineChar);
	if (ctx) return contextStr(ctx, {ctx});
        return "";
    };

    auto reportUnknownMsg = [&](bool isError, std::string msg) {
        replace(msg, lineTerm, "\n");
        translateAllLocs(msg);
        msg = (isError? errorColored("error:") : warnColored("warning:")) + " " + msg + "\n";
        reportMsg(isError, msg);
    };

    for (regex_it rit(str.begin(), str.end(), evRegex); rit != rend; rit++) {
        auto& m = *rit;
        bool isError = std::string(m[1]) == "Error";
        std::string msg = m[2];
        std::smatch hdrMatch;
        if (!std::regex_search(msg, hdrMatch, hdrRegex)) {
            // Special-case not-found top-level error
            if (msg.find("Command line:") != std::string::npos && msg.find("Unbound variable `mk") != std::string::npos) {
                bool isModule = isupper(topLevel[0]);
                msg = errorColored("error:") + " cannot find top-level " + (isModule? "module" : "function") + " " + errorColored("'" + topLevel + "'");
                reportMsg(isError, msg);
            } else {
                reportUnknownMsg(isError, msg);
            }
            continue;
        }
        std::string file = hdrMatch[1];
        uint32_t line = atoi(hdrMatch[2].str().c_str());
        uint32_t lineChar = atoi(hdrMatch[3].str().c_str());
        std::string code = hdrMatch[4];
        std::string body = msg.substr(hdrMatch.length());
        if (file != "Translated.bsv") {
            reportUnknownMsg(isError, "in imported BSV file " + msg);
            continue;
        }

        replace(body, lineTerm, " ");
        replace(body, "  ", " ");
        std::string loc = translateLoc(line, lineChar);
        body = trim(body);
        std::string unprocessedBody = body;
        if (body.size()) body[0] = tolower(body[0]);
        auto locToPos = translateAllLocs(body);

        // Find and highlight syntax elements
        std::vector<std::string> elems;
        std::regex elemRegex("`(.*?)'");
        std::smatch elemMatch;
        while (std::regex_search(body, elemMatch, elemRegex)) {
            std::string elem = elemMatch[1];
            // Translate all module constructors back to the module name
            if (elem.size() > 2 && elem.find("mk") == 0 && isupper(elem[2]))
                elem = elem.substr(2);
	    elems.push_back(elem);
	    replace(body, elemMatch[0], errorColored("'" + elem + "'"));
        }

        // Special-case a few codes; these rewrite body on success, o/w they fall through the default code
        if (code == "T0020" || code == "T0080") {
            // NOTE: T0020 is for expressions and T0080 is for functions, but
            // Bluespec seems to implement several constant as functions (e.g.,
            // True and False). So, we output exactly the same error message
            // for both
            std::regex typeRegex((code == "T0020")? "type error at: (.*?) Expected type: (.*?) Inferred type: (.*?)$" : "type error at the use of the following function: (.*?) The expected return type of the function: (.*?) The return type according to the use: (.*?)$");
            std::smatch match;
            if (std::regex_search(body, match, typeRegex)) {
                std::string elem = match[1];
                std::string expectedType = match[2];
                std::string type = match[3];
                body = "expression " + errorColored("'" + elem + "'") + " has type " + hlColored(type) + ", but use requires type " + hlColored(expectedType);
                elems.push_back(elem);
            }
        } else if (code == "T0031" || code == "T0032") {
            // First, find if the compiler is pinpointing an expression.
            // If so, use the expression loc as the loc, as that is,
            // by observation, more accurate.
            // NOTE: We used to do this only for T0032, where the default loc
            // is always way off---the beginning of the offending module. But
            // for some T0031s we've found the default loc to be bad too, so
            // just do it always. If the location is perplexing in some cases,
            // we could do more detailed analysis to see when the default loc
            // is bad (e.g., it's untranslated)
            std::regex exprRegex(" The proviso was implied by expressions at the following positions: (\\S+)");
            std::smatch exprMatch;
            if (std::regex_search(body, exprMatch, exprRegex)) {
                std::string exprLoc = exprMatch[1];
                bool isLoc = locToPos.find(exprLoc) != locToPos.end();
                bool isMinispec = exprLoc.find("(translated") == std::string::npos;
                if (isLoc && isMinispec) {
                    loc = exprLoc;
                    std::tie(line, lineChar) = locToPos[exprLoc];
                    replace(body, exprMatch[0], "");  // take it out
                }
            }

	    // Then, handle the actual proviso error
	    std::regex provisoRegex((code == "T0031")?
                        "no instances of the form:\\s+(\\S+?)#\\((.*)\\)" :
                        "proviso which could not be resolved:\\s+(\\S+?)#\\((.*)\\)");
            std::smatch match;
            if (std::regex_search(body, match, provisoRegex)) {
                std::string typeclass = match[1];
                std::string type = match[2];
                if (typeclass == "Arith") {
                    body = "type " + hlColored(type) + " does not support arithmetic operations";
                } else if (typeclass == "Ord") {
                    body = "type " + hlColored(type) + " does not support comparison operations";
                } else if (typeclass == "Literal") {
                    body = "cannot convert literal to type " + hlColored(type);
                } else if (typeclass == "FShow") {
                    body = "cannot display value of type " + hlColored(type);
                    if (type.find("function") == 0)
                        body += " (this is a function, did you forget some/all the arguments?)";
                } else if (typeclass == "Bits") {
                    std::regex bitsRegex("(.*?), (\\S+)");
                    std::smatch bitsMatch;
                    if (std::regex_search(type, bitsMatch, bitsRegex)) {
                        std::string badType = bitsMatch[1];
                        std::string length = bitsMatch[2];
                        body = "type " + errorColored("'" + badType + "'") + " cannot be used here";
                        if (badType == "Integer") {
                            body += " because Integer is a compile-time-only type with an unbounded number of bits, so it can't be synthesized to hardware";
                        } else if (length == "a__") {
                            // FIXME: This happens in Reg#(), but seems very tailored.
                            body += " because this type is not synthesizable to bits, which is required by the use";
                        } else {
                            body += " because either this type is not synthesizable to bits, or it has a bit-width incompatible with its use";
                        }
                    }
                } else if (typeclass == "Add") {
                    body = "expression type has a number of bits or elements incompatible with its use";
                    std::regex addRegex("(\\d+), (\\d+), (\\d+)");
                    std::smatch addMatch;
                    if (std::regex_search(type, addMatch, addRegex)) {
                        std::string n1 = addMatch[1];
                        std::string n2 = addMatch[2];
                        std::string n3 = addMatch[3];
                        body += " (for lengths to match, "  + n1 + " + " + n2 + " should equal " + n3 + ")";
                    }
                    // All the "overlength" expressions I've seen so far (e.g.,
                    // concatenation) follow this format; if you see something
                    // else, e.g., Add#(a__, 1, 0), generalize this regex.
                    std::regex overRegex("(\\d+), (\\S+)_, 0");
                    if (std::regex_search(type, addMatch, overRegex)) {
                        std::string n1 = addMatch[1];
                        body += " (expression has " + n1 + " more/fewer bits or elements than its use allows)";
                    }
                }
            }
	} else if (code == "T0003") {
	    // I see these only on mistyped literals, but unbound constructor
	    // is such a general message that who knows where else it may show
	    // up. So leave the translated error general.
	    replace(body, "unbound constructor", "undefined literal, type, or module");
        } else if (code == "T0004") {
	    replace(body, "unbound variable", "undefined variable or function");
	} else if (code == "T0007") {
	    replace(body, "unbound type constructor", "undefined type or module");
        } else if (code == "T0016") {
            // Error message is good, except when it's an input, so process only that
            std::regex inputRegex("Field `(.*?)___input' is not in the type `(.*?)' which was derived for this expression");
            std::smatch match;
            if (std::regex_search(unprocessedBody, match, inputRegex)) {
                std::string input = match[1];
                std::string modType = match[2];
                body = "module " + hlColored(modType) + " does not have an input named " + errorColored("'" + input + "'");
            }
        } else if (code == "T0081" || code == "T0083" || code == "T0084") {
            // Errors related to using a function with the wrong number of arguments
            // First, the expected/inferred type trailing info is more confusing then helpful (function type noise). So take that out.
            std::string trailMarker = (code == "T0081")? " Expected type:" : " The expected type is:";
            auto trailStart = body.find(trailMarker);
            body = body.substr(0, trailStart);  // safe even if trail is string::npos
            // Second, if the function is actually a module, don't call it a function :)
            if (body.find(": mk") != std::string::npos) {
                replace(body, ": mk", ": ");
                replace(body, "function", "module");
            }
        } else if (code == "G0004") {
            // Register double-writes and input/wire double-sets
            std::regex conflictRegex("Rule `(.*?)' uses methods that conflict in parallel: (.*?)(\\S+) and (.*?)(\\S+) For the complete expressions");
            std::smatch match;
            if (std::regex_search(unprocessedBody, match, conflictRegex)) {
                std::string rule = match[1];
                // g1/g2 are the "guards" and may be empty; m1 and m2 are the methods
                std::string g1 = match[2];
                std::string m1 = match[3];
                std::string g2 = match[4];
                std::string m2 = match[5];
                bool isWrite = m1.find(".write") != std::string::npos;
                bool isWset = m1.find(".wset") != std::string::npos;
                bool isWget2 = m2.find(".wget") != std::string::npos;
                bool isWhas2 = m2.find(".whas") != std::string::npos;
                std::string base1 = "'" + m1.substr(0, m1.find(".")) + "'";
                std::string base2 = "'" + m2.substr(0, m1.find(".")) + "'";

                body = "rule " + errorColored("'" + rule + "'") + " ";
                if (m1 == m2 && (isWrite || isWset)) {
                    if (isWrite) {
                        body += "writes to register " + errorColored(base1) + " more than once, which is forbidden";
                    } else {
                        assert(isWset);
                        body += "sets input or wire " + errorColored(base1) + " more than once, which is forbidden";
                    }
                    // Non-disjoint if statements are confusing, so clarify
                    if (g1 == g2 || g1 == "if (...) ")
                        body += "; these happen inside if statements that have overlapping predicates (make the if statements mutually exclusive, so that they never take effect on the same cycle)";
                } else if (isWset && isWget2 && base1 == base2) {
                    body += "both sets input or wire " + errorColored(base1) + ", and reads from it (perhaps through a method), which is forbidden";
                } else if (isWset && isWhas2 && base1 == base2) {
                    // NOTE(dsm): whas seems to always fire with wget; print them separately though, in case there's a wset/whas conflict but not wset/wget
                    body += "both sets input or wire " + errorColored(base1) + " (which has a default value), and reads from it (perhaps through a method), which is forbidden";
                } else {
                    // Print a generic message, this must be interacting with Bluespec code
                    body += "cannot call methods " + errorColored(m1) + " and " + errorColored(m2) + " because they conflict";
                }
            }
        } else if (code == "G0005") {
            // Minispec rules must fire every cycle
            std::regex blockedRegex("The assertion `fire_when_enabled' failed for rule `(.*?)' because it is blocked by rule (.*?) in the scheduler");
            std::smatch match;
            if (std::regex_search(unprocessedBody, match, blockedRegex)) {
                body = "rules " + errorColored(match[1]) + " and " + errorColored(match[2]) +
                    " conflict and cannot both fire every cycle (e.g., they both try to set the same input of a shared module)";
            }
        } else if (code == "G0066") {
            std::regex unsetRegex("Instance `(.*?)' requires the following method to be always enabled");
            std::smatch match;
            if (std::regex_search(unprocessedBody, match, unsetRegex)) {
                std::string instance = match[1];
                body = "input or wire " + errorColored("'" + instance + "'") + " has no default value, so it must be set every cycle, but it is never being set";
            }
        } else if (code == "G0015") {
            std::regex unsetRegex("Instance `(.*?)' requires the following method to be always enabled, but the condition for executing the method could not be proven to be always True: _write");
            std::smatch match;
            if (std::regex_search(unprocessedBody, match, unsetRegex)) {
                // This is a warning, but its gravity is context-dependent. If
                // we're producing Verilog, then it should stay a warning (or
                // go away); if this is simulation, then we must promote it to
                // an error, as it'll actually cause things to misbehave.
                isError = simOut;
                std::string instance = match[1];
                body = "input or wire " + errorColored("'" + instance + "'") + " has no default value, so it must be set every cycle, but it is being set only sometimes (at least, I cannot prove that a rule is setting it every cycle; simplify your control flow or add a default value); note: this warning is promoted to an error when producing simulation executables";
            }
        }

        // Simplify bsc output: Translated::TypeName -> TypeName, etc.
        replace(body, "Translated::", "");
        replace(body, "Vector::Vector", "Vector");

        std::stringstream ss;
        ss << hlColored(loc + ":") << " " << (isError? errorColored("error:") : warnColored("warning:")) << " " << body << "\n";
        ss << contextStrFn(line, lineChar, elems);
        //ss << code;
        reportMsg(isError, ss.str(), sm.getContextInfo(line, lineChar), sm.find(line, lineChar));
    }
}

struct RunResult {
    std::string output;
    int exitCode;
};

RunResult run(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) error("cannot invoke subprocess");
    std::stringstream bscOutput;
    size_t bufSize = 1024;
    char buf[bufSize];
    while (!feof(pipe)) if (fgets(buf, bufSize, pipe)) bscOutput << buf;
    RunResult res;
    res.output = bscOutput.str();
    res.exitCode = pclose(pipe);
    return res;
}

static std::string tmpDirStr = "";
void cleanupTmpDir() {
    if (!tmpDirStr.size()) return;
    std::filesystem::remove_all(tmpDirStr);
}

[[noreturn]] void uncaughtExceptionHandler() noexcept {
    // dsm: Why is C++ so retarded? rethrow?
    std::string exStr = "??";
    std::exception_ptr ep = std::current_exception();
    try {
        if (ep) {
            std::rethrow_exception(ep);
        } else {
            exStr = "invalid exception";
        }
    } catch(const std::exception& e) {
        exStr = e.what();
    }
    panic("uncaught exception: %s", exStr.c_str());
}

int main(int argc, const char* argv[]) {
    std::set_terminate(uncaughtExceptionHandler);

    argparse::ArgumentParser args;
    args.add_argument("inputFile")
        .help("input file")
        .default_value(std::string(""));
    args.add_argument("topLevel")
        .help("name of module/function to compile (if not given, checks input for correctness)")
        .default_value(std::string(""));
    args.add_argument("-o", "--output")
        .help("type of output(s) desired [default: sim]\n                  sim: simulation executable\n                  verilog (or v): Verilog file\n                  bsv: Bluespec file\n                  Use commas to specify multiple outputs (e.g., -o sim,verilog)")
        .default_value(std::string("sim"));
    args.add_argument("-p", "--path")
        .help("path for source files (for multiple directories, use : as separator)")
        .default_value(std::string(""));
    args.add_argument("-b", "--bscOpts")
        .help("extra options for the Bluespec compiler (use quotes for multiple options)")
        .default_value(std::string(""));
    args.add_argument("-v", "--version")
        .help("show version information")
        .default_value(false)
        .implicit_value(true);
    args.add_argument("--all-errors")
        .help("report all errors and warnings (by default, similar/repeating errors are filtered)")
        .default_value(false)
        .implicit_value(true);
    args.add_argument("--keep-tmps")
        .help("keep temporary files around (useful for compiler debugging)")
        .default_value(false)
        .implicit_value(true);
    args.add_argument("--max-elab-steps")
        .help("maximum number of elaboration steps")
        .default_value((uint64_t) 50000)
        .scan<'u', uint64_t>();
    args.add_argument("--max-elab-depth")
        .help("maximum elaboration depth")
        .default_value((uint64_t) 1000)
        .scan<'u', uint64_t>();

    try {
        args.parse_args(argc, argv);
    } catch (const std::exception& err) {
        error("could not parse command-line arguments: %s\n       run %s --help for information on command-line options",
                err.what(), argv[0]);
    }

    if (args.get<bool>("--version")) {
        std::cout << "Minispec compiler version " << getVersion() << "\n";
        exit(0);
    }

    std::string inputFile = args.get<std::string>("inputFile");
    if (inputFile == "") error("no input file");
    std::string topLevel = args.get<std::string>("topLevel");

    // Find desired outputs
    bool bsvOut = false;
    bool simOut = false;
    bool verilogOut = false;
    bool defaultOut = !args.is_used("--output");
    {
        std::string outsArg = args.get<std::string>("--output");
        std::string outs = outsArg;
        replace(outs, ",", " ");
        std::istringstream iss(outs);
        std::string out;
        while (iss >> out) {
            if (out == "bsv") bsvOut = true;
            else if (out == "sim") simOut = true;
            else if (out == "verilog" || out == "v") verilogOut = true;
            else error("invalid output type %s (full argument: %s)",
                    errorColored("'" + out + "'").c_str(),
                    errorColored("'" + outsArg + "'").c_str());
        }
    }

    // Other options
    initReporting(args.get<bool>("--all-errors"));
    setElabLimits(args.get<uint64_t>("--max-elab-steps"), args.get<uint64_t>("--max-elab-depth"));

    // Construct the Minispec path, composed of: (1) the input file's
    // directory, (2) the directories in the --path flag, and (3) the current
    // directory. This way we catch current-folder includes to avoid some
    // corner cases, but without clobbering same-dir includes.
    std::vector<std::string> path;
    path.push_back(std::filesystem::path(inputFile).remove_filename());
    std::stringstream pathSs(args.get<std::string>("--path"));
    for (std::string dir; std::getline(pathSs, dir, ':'); )
        path.push_back(dir);
    path.push_back("");

    auto dedup = [](const std::vector<std::string>& v) {
        std::vector<std::string> res;
        std::unordered_set<std::string> elems;
        for (auto e : v) if (!elems.count(e)) {
            elems.insert(e);
            res.push_back(e);
        }
        return res;
    };
    path = dedup(path);

    // Parse all files. Exits on lexer/parser errors.
    std::vector<MinispecParser::PackageDefContext*> parsedTrees =
        parseFileAndImports(inputFile, path);

    // Translate files to Bluespec. Exits on elaboration errors.
    SourceMap sm = translateFiles(parsedTrees, topLevel);

    // Save translated code
    char tmpDir[128];
    sprintf(tmpDir, "tmp_msc_XXXXXX");
    if (mkdtemp(tmpDir) != tmpDir) error("could not create temporary directory");
    if (args.get<bool>("--keep-tmps")) {
        std::cout << "storing temporary files in " << hlColored(std::string(tmpDir)) << "\n";
    } else {
        tmpDirStr = tmpDir;
        atexit(cleanupTmpDir);
    }
    std::string bsvFileName = tmpDir + std::string("/Translated.bsv");
    std::ofstream stream(bsvFileName);
    if (!stream.good()) error("Could not open output file %s", bsvFileName.c_str());
    stream << sm.getCode() << "\n";
    stream.close();

    // bsc path is simply the path with a corrected base for relative dirs
    std::stringstream bscPath;
    for (std::string dir : path) {
        auto path = std::filesystem::path(dir);
        bscPath << (path.is_relative()? "'../" : "'") << dir << "':";
    }
    bscPath << "%:+";
    std::string bscOpts = "-p " + bscPath.str() + " " + args.get<std::string>("--bscOpts");
    //std::cout << "BSC options: " << bscOpts << "\n";

    // Invoke Bluespec compiler and check for type errors
    auto runBscCmd = [&](const std::string& cmd) {
        //std::cout << cmd << "\n";
        auto compileRes = run(cmd);
        reportBluespecOutput(compileRes.output, sm, topLevel, simOut);
        exitIfErrors();
	if (compileRes.exitCode != 0) {
            // If we didn't parse any error but bsc failed, this is typically
            // because bsc wasn't found. So print the output.
            error("could not compile file: %s", compileRes.output.c_str());
        }
    };

    std::string outName = topLevel;
    if (outName == "") {
        outName = std::filesystem::path(inputFile).stem();
    } else {
        // Sanitize parametrics
        replace(outName, "#", "_");
        replace(outName, ",", "_");
        replace(outName, "(", "");
        replace(outName, ")", "");
        replace(outName, " ", "");
        replace(outName, "'", "");
        replace(outName, "\t", "");
    }
    bool typechecked = false;

    if (simOut) {
        if (topLevel.size() && isupper(topLevel[0])) {
            std::stringstream cmd;
            cmd << "(cd " << tmpDir << " && bsc " << bscOpts << " -sim -g '" << sm.getTopModule() << "' -u Translated.bsv) 2>&1 >/dev/null";
            runBscCmd(cmd.str());
            typechecked = true;

            // Link simulation executable
            cmd.str("");
            cmd << "(cd " << tmpDir << " && bsc " << bscOpts << " -sim -e '" <<  sm.getTopModule() << "' -o '../" << outName << "') 2>&1 >/dev/null";
            runBscCmd(cmd.str());
            std::cout << "produced simulation executable " << hlColored(outName) << "\n";
        } else if (!defaultOut) {
            const char* problem = (topLevel == "")?
                "did not provide a top-level module" :
                "specified a top-level function, which can't be simulated";
            warn("you asked for sim output but %s, so not producing simulation executable", problem);
        }
    }

    if (verilogOut) {
        if (topLevel.size()) {
            std::stringstream cmd;
            cmd << "(cd " << tmpDir << " && bsc " << bscOpts << " -verilog -D __VERILOG__ -g '" << sm.getTopModule() << "' -u Translated.bsv) 2>&1 >/dev/null";
            runBscCmd(cmd.str());
            typechecked = true;

            cmd.str("");
            cmd << "cp '" << tmpDir << "/" << sm.getTopModule() << ".v' '" << outName << ".v'";
            run(cmd.str());
            std::cout << "produced verilog output " << hlColored(outName + ".v") << "\n";
        } else if (!defaultOut) {
            warn("you asked for verilog output but did not provide a top-level module or function, so not producing verilog");
        }
    }

    if (!typechecked) {
        std::stringstream cmd;
        cmd << "(cd " << tmpDir << " && bsc " << bscOpts << " -u Translated.bsv) 2>&1 >/dev/null";
        runBscCmd(cmd.str());
        typechecked = true;
        std::cout << "no errors found on " << hlColored(inputFile) << "\n";
    }

    if (bsvOut) {
        auto cpRes = run("cp " + std::string(tmpDir) + "/Translated.bsv '" + outName + ".bsv'");
        if (cpRes.exitCode != 0) {
            error("could not copy bsv file");
        }
        std::cout << "produced bsv output " << hlColored(outName + ".bsv") << "\n";
    }

    return 0;
}
