FROM ros

COPY . /MSF_LOAM
CMD bash /MSF_LOAM/install.sh
