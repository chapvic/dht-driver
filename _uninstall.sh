#!/bin/bash

sudo rmmod dht 2>/dev/nul
sudo make uninstall
make clean
