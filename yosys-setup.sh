wget -nc -nv https://github.com/YosysHQ/yosys/archive/yosys-0.8.tar.gz
tar xzf yosys-0.8.tar.gz
cd yosys-yosys-0.8 && make -j16 yosys-abc
cd abc
git apply /vagrant/synth/abc.patch
make clean
cd ..
sed -i "s/ABCREV = ae6716b/ABCREV = default/g" Makefile
make clean && PREFIX=/home/vagrant/yosys make -j16 install