#!/usr/bin/bash

set -e

mv /etc/apt/sources.list /etc/apt/sources.list.save
cat << EOF > /etc/apt/sources.list
deb http://mirrors.aliyun.com/ubuntu/ bionic main restricted universe multiverse
deb http://mirrors.aliyun.com/ubuntu/ bionic-security main restricted universe multiverse
deb http://mirrors.aliyun.com/ubuntu/ bionic-updates main restricted universe multiverse
deb http://mirrors.aliyun.com/ubuntu/ bionic-proposed main restricted universe multiverse
deb http://mirrors.aliyun.com/ubuntu/ bionic-backports main restricted universe multiverse
EOF

apt update

apt install vim cmake ninja-build ros-melodic-image_transport ros-melodic-cv-bridge ccache ros-melodic-tf libceres-dev libpcl-dev ros-melodic-pcl-conversions

echo "source /opt/ros/melodic/setup.bash" > ~/.bashrc
