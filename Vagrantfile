# SPDX-License-Identifier: GPL-2.0

#
# This file is part of Lustre, http://www.lustre.org/
#
# contrib/coverity/Vagrantfile
#
# Vagrant definition for a CentOS VM to run a Lustre
# build for Coverity.
#
# Author: Timothy Day <timday@amazon.com>
#

Vagrant.configure("2") do |config|
  # The most common configuration options are documented and commented below.
  # For a complete reference, please see the online documentation at
  # https://docs.vagrantup.com.

  # Every Vagrant development environment requires a box. You can search for
  # boxes at https://vagrantcloud.com/search.
  config.vm.box = "centos/8"

  # Customizations
  config.vm.provider "libvirt" do |libvirt|
    libvirt.machine_virtual_size = 40
    libvirt.memory = 8192
    libvirt.cpus = 4
  end

  config.vm.provision "shell", inline: <<-SHELL
      # Volume Setup
      sed -i -e "s|mirrorlist=|#mirrorlist=|g" /etc/yum.repos.d/CentOS-*
      sed -i -e "s|#baseurl=http://mirror.centos.org|baseurl=http://vault.centos.org|g" /etc/yum.repos.d/CentOS-*
      dnf update -y
      dnf install -y cloud-utils-growpart
      growpart /dev/vda 1
      xfs_growfs /dev/vda1

      # Networking Setup
      GIVEN_IP=$(ip address show eth0 | awk -F' ' '$1 == "inet" { print $2 }' | awk -F'/' '{ print $1 }')
      grep $(hostname) /etc/hosts
      sed -i "s/$(hostname) //g" /etc/hosts
      echo "$GIVEN_IP $(hostname)" >> /etc/hosts
      grep $(hostname) /etc/hosts
  SHELL
end
