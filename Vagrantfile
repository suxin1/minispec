# -*- mode: ruby -*-
# vi: set ft=ruby :
def total_cpus
  require 'etc'
  Etc.nprocessors
end

Vagrant.configure("2") do |config|
  config.vm.define "minispec"
  config.vm.hostname = "minispec"
#   config.vm.box = "bento/ubuntu-20.04"
  config.vm.box = "tknerr/baseimage-ubuntu-20.04"

  config.vm.provider "docker" do |vb|
#     vb.gui = true
#     vb.name = "minispec"
#     vb.memory = "4096"
#     vb.cpus = total_cpus / 2
#     vb.default_nic_type = "virtio"
#     vb.customize ["modifyvm", :id, "--paravirtprovider", "kvm"] # faster for linux guests
  end
  # For jupyter notebooks
  config.vm.network "forwarded_port", guest: 8888, host: 8888
#   config.vm.synced_folder ".", "/vagrant"

  config.vm.provision "shell", inline: <<-SHELL
    # Packages
    export DEBIAN_FRONTEND=noninteractive
    apt-get -y update

    # Basics, direct minispec deps (include bsc deps), antlr deps, antlr runtime build
    apt-get -y install vim  scons git build-essential g++ libxft2 libgmp10  openjdk-11-jdk-headless  cmake pkg-config uuid-dev


    # Download bsc
    BSVER="bsc-2021.07-ubuntu-20.04"
    BSREL="2021.07"
    if [ ! -d ~vagrant/${BSVER} ]; then
      echo "Downloading bsc ${BSREL} / ${BSVER}"
      sudo -u vagrant wget -nc -nv https://github.com/B-Lang-org/bsc/releases/download/${BSREL}/${BSVER}.tar.gz
      sudo -u vagrant tar xzf ${BSVER}.tar.gz
      echo "# Bluespec config" >> ~vagrant/.bashrc
      echo 'BSPATH=\$HOME/'${BSVER} >> ~vagrant/.bashrc
      echo 'export BLUESPECDIR=\$BSPATH/lib' >> ~vagrant/.bashrc
      echo 'export PATH=\$BSPATH/bin:\$PATH' >> ~vagrant/.bashrc
    fi

    # Yosys
    apt-get -y install build-essential clang bison flex libreadline-dev gawk tcl-dev libffi-dev pkg-config python3 graphviz
    if [ ! -d ~vagrant/yosys ]; then
      echo "Downloading yosys"
      sudo -u vagrant /vagrant/synth.build-yosys.sh
      sudo -u vagrant mv yosys-yosys-0.8 yosys
      echo "# Yosys config" >> ~vagrant/.bashrc
      echo 'export PATH=\$HOME/yosys:\$PATH' >> ~vagrant/.bashrc
    fi

    # netlistsvg (for synth)
    apt-get -y install npm
    npm install -g netlistsvg
    
    # Jupyter notebook (locally, from source, with Minispec syntax)
    apt-get -y install python3-pip
    if [ ! -d ~vagrant/notebook-5.7.8 ]; then
      pip install --upgrade setuptools pip
      # Install jupyter notebook with minispec syntax patch
      sudo chmod +x /vagrant/jupyter/install-jupyter.sh
      sudo -H -u vagrant /vagrant/jupyter/install-jupyter.sh
      # Link minispec kernel so it can be used as a module
      # (alternatively, change kernel def to include full path to minispeckerenel.py)
      ln -s /vagrant/jupyter/minispeckernel.py /usr/local/lib/python3.8/dist-packages/
      # Install minispec jupyter kernel
      jupyter kernelspec install /vagrant/jupyter/kernel/minispec/
    fi

    # Finally, build minispec itself (note: in shared folder... might want to
    # move repo somewhere to make VM fully self-contained)
    if [ ! -d /vagrant/msc ]; then
      sudo -H -u vagrant bash -c "cd /vagrant && scons -j4"
      # vboxsf suffers from horrible racy behavior, so sometimes the antlr4
      # download & install code fails because the untarred antlr4 source takes
      # a bit to become available (yep, it's that inconsistent). So try twice
      sleep 1
      sudo -H -u vagrant bash -c "cd /vagrant && scons -j4"
      echo 'export PATH=/vagrant:/vagrant/synth:\$PATH' >> ~vagrant/.bashrc
    fi

  SHELL
end
