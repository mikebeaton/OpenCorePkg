#!/bin/bash

ps -a | grep build | grep -v "$0" | grep -v "grep" | awk '{print $1}' | xargs kill -9
