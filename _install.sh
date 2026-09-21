#!/bin/bash

./_uninstall.sh
sudo make install && sudo modprobe dht
