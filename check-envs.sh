#!/usr/bin/env bash

pkgs_req=(x11-utils mesa-utils)

### Help functions
debug() {
    if [ ! -z "$DEBUG" ]; then
        local msg="$1"; shift
        printf "==> \033[38;5;196m${msg}\033[00m\n" "$@" >&2
    fi
}

msg() {
    if [ ! -z "$DEBUG" ]; then
        local msg="$1"; shift
        printf "==> \033[38;5;118m${msg}\033[00m\n" "$@" >&2
    fi
}


# direct rendering == no 通常表示驱动安装有问题
# 但是direct rendering 并不能保证一定有硬件加速
check_glx() {
    local case1="direct rendering"
    local exp1="yes"

    local case2="OpenGL renderer string"
    local exp2="llvmpipe|swrast|software rasterizer"

    local case3="OpenGL version string"
    local exp3="^1\.[0-9]"

    local N=3

    if ! `glxinfo >/dev/null 2>&1`; then 
        msg "glxinfo failed"
        exit 1; 
    fi

    for i in `seq $N`; do 
        eval case=\$case$i
        eval exp=\$exp$i
        debug "testing ${case}"

        glxinfo | awk -F: ' $1 ~ /'"$case"'/ { if ($2 ~ /'"$exp"'/) exit 1 }'
        if [ $? -gt 0 ]; then
            exit 1
        fi

        debug "pass"
    done
}

#TODO: need to check and review xdriinfo options driver?
check_dri() {
    debug "testing dri device"
    if [ ! -c /dev/dri/card0 ]; then exit 1; fi
    debug "pass"

    local case1="Screen"
    local exp1="not direct rendering capable"

    local N=1

    if ! `xdriinfo >/dev/null 2>&1`; then 
        msg "xdriinfo failed"
        exit 1; 
    fi

    for i in `seq $N`; do 
        eval case=\$case$i
        eval exp=\$exp$i
        debug "testing ${case}"

        xdriinfo | awk -F: ' $1 ~ /'"$case"'/ { if ($2 ~ /'"$exp"'/) exit 1 }'
        if [ $? -gt 0 ]; then
            exit 1
        fi

        debug "pass"
    done
}

check_drmkms() {
    #TODO: check kernel cmdline to see if kms disabled 
    # either explicitly modeset=0 or vga=xxx video=xxx vesafb=xxx uvesafb=xxx
}

#TODO: check if commands needed installed

check_glx
check_dri

