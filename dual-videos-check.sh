#!/usr/bin/env bash
# check dual graphics video card setup
# this test works when DISPLAY is set and need to run under root
# access some priviledged files.

DEBUG=1

### Help functions
bad() {
    if [ ! -z "$DEBUG" ]; then
        local msg="$1"; shift
        printf "==> \033[38;5;199m${msg}\033[00m %s\n" "$*" >&2
    fi
}

debug() {
    if [ ! -z "$DEBUG" ]; then
        local msg="$1"; shift
        printf "==> \033[38;5;196m${msg}\033[00m %s\n" "$*" >&2
    fi
}

msg() {
    if [ ! -z "$DEBUG" ]; then
        local msg="$1"; shift
        printf "==> \033[38;5;118m${msg}\033[00m %s\n" "$*" >&2
    fi
}

has_dual_cards() {
    local count

    count=`lspci | grep -iE 'VGA|3D' | wc -l`
    [[ $count -gt 1 ]] && return 0 || return 1
}

has_vgaswitcheroo() {
    bash -c '[[ -e /sys/kernel/debug/vgaswitcheroo/switch ]]'
}

# FIXME: how to know it?
has_optimus() {
    false
}

# parse /sys/kernel/debug/vgaswitcheroo/switch and assign to global args
parse_vgaswitch_file() {
    local switch_file oldifs 

    switch_file=/sys/kernel/debug/vgaswitcheroo/switch

    oldifs=$IFS
    IFS=:
    while read id ty status pwr domain bus slot; do
        if [ x$status == x+ ]; then
            status=1
        else
            status=0
        fi

        case $ty in
            DIS)  DIS_STATUS=$status; DIS_BUS=$bus:$slot;;
            IGD)  IGD_STATUS=$status; IGD_BUS=$bus:$slot;;
        esac

        #echo $ty $status $bus:$slot
    done < $switch_file

    IFS=$oldifs
}

parse_optimus() {
    :
}

# return names of providers (consider first is active)
get_provider_names() {
    #I+A
    #Providers: number : 2
    #Provider 0: id: 0x7c cap: 0xb, Source Output, Sink Output, Sink Offload crtcs: 4 outputs: 5 associated providers: 0 name:Intel
    #Provider 1: id: 0x53 cap: 0xf, Source Output, Sink Output, Source Offload, Sink Offload crtcs: 4 outputs: 0 associated providers: 0 name:radeon

    #I+N
    #Providers: number : 2
    #Provider 0: id: 0xae cap: 0x7, Source Output, Sink Output, Source Offload crtcs: 4 outputs: 4 associated providers: 0 name:nouveau
    #Provider 1: id: 0x45 cap: 0xb, Source Output, Sink Output, Sink Offload crtcs: 4 outputs: 2 associated providers: 0 name:Intel

    #I+N(with proprietary driver)
    #Providers: number : 2
    #Provider 0: id: 0x2c0 cap: 0x1, Source Output crtcs: 4 outputs: 6 associated providers: 0 name:NVIDIA-0
    #Provider 1: id: 0x45 cap: 0xb, Source Output, Sink Output, Sink Offload crtcs: 4 outputs: 2 associated providers: 0 name:Intel


    #FIXME: I assume the first listed is active provider
    #since I have no way to find out which is active, maybe use name as heuristics
    awk ' NR>1 {
        sub(/^[^:]+\s*:/, ""); 
        total = patsplit($0, a, /(\w+:\s*\S+)/);
        for (i = 1; i <= total; i++) {
            if (a[i] ~ /name:/) {
                sub(/name:/, "", a[i]);
                print a[i] 
            }
        }
    } ' <(xrandr --listproviders)
}

# we assume no more than two cards exists
get_inactive_card_path() {
    local id id2
    eval id="\${${ACTIVE_CARD}_DRMID}"
    [[ $id -eq 0 ]] && id2=1 || id2=0
    echo -n "/sys/class/drm/card$id2"
}
get_active_card_path() {
    local id
    eval id="\${${ACTIVE_CARD}_DRMID}"
    echo -n "/sys/class/drm/card$id"
}

# this only works if a card has connectors, 
# and xrandr list connectors of current *source output*
#
# CAVEAT: when nvidia proprietary driver installed, connectors of discrete 
# card is missing from /sys/class/drm (WTF)
parse_provider_by_outputs() {
    declare -a conn names
    local oldifs

    # the reason I split connector name into pieces is that xrandr may shows as DP1
    # while drm may shows as DP-1
    # FIXME: there is a possibility that the two names does not match at all. 
    # (e.g HDMI1 vs HDMI-A-1). need to improve.
    conn=( `awk '
        $0 ~ /connected/ {
        n = patsplit($1, a, /[a-zA-Z]+|[0-9]+/)
        for (i = 0; i <= n; i++)
            print a[i]
            exit 
        }' <(xrandr)` )

    names=(`get_provider_names`)

    echo ${conn[@]}
    oldifs=$IFS

    # if no connectors at all (connected or disconnected), consider inactive
    if [[ -z "$conn" ]]; then
        return 0
    fi

    card=`get_active_card_path`

    IFS=""
    if [[ -e "$card-${conn[*]}" ]]; then
        IFS=$oldifs
        eval ${ACTIVE_CARD}_NAME="${names[0]}"
        eval ${INACTIVE_CARD}_NAME="${names[1]}"
        return 0
    else
        IFS="-"
        if [[ -e "$card-${conn[*]}" ]]; then
            IFS=$oldifs
            eval ${ACTIVE_CARD}_NAME="${names[0]}"
            eval ${INACTIVE_CARD}_NAME="${names[1]}"
            return 0
        fi
    fi
    IFS=$oldifs

    #FIXME: if we cannot deduce active card from connectors, just set names anyway.
    # I believe this is correct.
    if [[ -z "$DIS_NAME" ]]; then
        eval ${ACTIVE_CARD}_NAME="${names[0]}"
        eval ${INACTIVE_CARD}_NAME="${names[1]}"
    fi
}

# incase no vgaswitcheroo file or others, simply assume card on bus 0 is IGD and active.
parse_sysfs_for_info() {
    local bus_desc bus bus_id driver enabled drm_id

    for card in /sys/class/drm/card?; do 
        bus_desc=$(basename $(readlink "$card/device"))
        bus=${bus_desc#*:}
        bus_id=${bus%:*}
        drm_id=${card:(-1)}

        read boot < $card/device/boot_vga 
        driver=$(basename $(readlink "$card/device/driver"))
        read enabled < $card/device/enable
        msg "enabled " $enabled

        if [ $bus_id -eq 0 ]; then
            IGD_BUS=$bus
        else 
            DIS_BUS=$bus
        fi

        if [[ "${bus_desc#*:}" == "$DIS_BUS" ]]; then
            if [[ $boot -eq 1 ]]; then
                ACTIVE_CARD=DIS 
                INACTIVE_CARD=IGD
            fi
            DIS_DRMID=$drm_id
            DIS_BOOT=$boot 
            if [[ -z $DIS_STATUS ]]; then
                DIS_STATUS=$enabled
            fi
            DIS_DRIVER=$driver

        elif [[ "${bus_desc#*:}" == "$IGD_BUS" ]]; then
            if [[ $boot -eq 1 ]]; then
                ACTIVE_CARD=IGD 
                INACTIVE_CARD=DIS
            fi
            IGD_DRMID=$drm_id
            IGD_BOOT=$boot 
            if [[ -z $IGD_STATUS ]]; then
                IGD_STATUS=$enabled
            fi
            IGD_DRIVER=$driver
        fi
    done
}

parse_offloading_capable() {
    declare -a dis_caps igd_caps

    dis_caps=(`awk '
            $0 ~ /name:'"$DIS_NAME"'/ {
                print (tolower($0) ~ "source offload" ? "1":"0")
                print (tolower($0) ~ "sink offload" ? "1":"0")
            }' <(xrandr --listproviders)`)

    igd_caps=(`awk '
            $0 ~ /name:'"$IGD_NAME"'/ {
                print (tolower($0) ~ "source offload" ? "1":"0")
                print (tolower($0) ~ "sink offload" ? "1":"0")
            }' <(xrandr --listproviders)`)

    DIS_SRC_OFFLOAD=${dis_caps[0]}
    DIS_SINK_OFFLOAD=${dis_caps[1]}
    IGD_SRC_OFFLOAD=${igd_caps[0]}
    IGD_SINK_OFFLOAD=${igd_caps[1]}
}

parse_cards_info() {
    if has_vgaswitcheroo; then
        parse_vgaswitch_file
    elif has_optimus; then
        parse_optimus
    fi

    # this is as default fallback
    parse_sysfs_for_info

    parse_provider_by_outputs

    parse_offloading_capable
}

#------------------------------------------------------------------------------

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
        debug "testing" "${case}"

        glxinfo | awk -F: ' $1 ~ /'"$case"'/ { if ($2 ~ /'"$exp"'/) exit 1 }'
        if [ $? -gt 0 ]; then
            bad "$case" failed
            exit 1
        fi

        debug "pass"
    done
}


# check if current driving device is $arg1
check_vgaswitcheroo_result() {
    local target=$1
    local switch_file oldifs ret=1

    switch_file=/sys/kernel/debug/vgaswitcheroo/switch

    oldifs=$IFS
    IFS=:
    while read id ty status pwr domain bus slot; do
        if [ x$status == x+ ]; then
            status=1
        else
            status=0
        fi

        if [[ $ty == $target && $status -eq 1 ]]; then
            break
        fi
    done < $switch_file

    IFS=$oldifs
    return $ret
}

test_switch_vgaswitcheroo() {
    local switch target origin

    switch=/sys/kernel/debug/vgaswitcheroo/switch

    if [[ x$DIS_STATUS -eq 1 ]]; then
        origin=DIS
        target=IGD

    else
        origin=IGD
        target=DIS
    fi

    echo ON > $switch
    echo $target > $switch
    echo OFF > $switch
    sleep .1

    if ! check_vgaswitcheroo_result $target; then
        bad "switch to IGD failed"
        exit 1
    fi
    #TODO: may be do basic dri check

    echo ON > $switch
    echo $origin > $switch
    echo OFF > $switch
}

test_switch_bumblebee() {
    :
}

test_switch() {
    [[ x$opt_dryrun == x1 ]] && return 0

    if has_vgaswitcheroo; then
        test_switch_vgaswitcheroo
    elif has_optimus; then
        test_switch_bumblebee
    fi
}

test_offload_rendering() {
    local provider sink offloading_capable dri

    eval provider="\${${INACTIVE_CARD}_NAME}"
    eval sink="\${${ACTIVE_CARD}_NAME}"

    eval offloading_capable="\${${INACTIVE_CARD}_SRC_OFFLOAD}"
    [[  x$offloading_capable != x1 ]] && return 0

    eval offloading_capable="\${${ACTIVE_CARD}_SINK_OFFLOAD}"
    [[  x$offloading_capable != x1 ]] && return 0

    [[ x$opt_dryrun == x1 ]] && return 0

    start_xserver

    xrandr --setprovideroffloadsink $provider $sink
    export DRI_PRIME=1
    check_glx

    dri=`xdriinfo | cut -d: -f2`
    case $provider in
        [Ii]ntel)
            if [[ $dri != i915 || $dri != i965 ]]; then
                bad "offloading seems failed"
                exit 1
            fi
            ;;
        NVIDIA*|nvidia*)
            if [[ $dri != nvidia ]]; then
                bad "offloading seems failed"
                exit 1
            fi
            ;;
        radeon)
            if [[ $dri != radeon ]]; then
                bad "offloading seems failed"
                exit 1
            fi
            ;;
        nouveau)
            if [[ $dri != nouveau ]]; then
                bad "offloading seems failed"
                exit 1
            fi
            ;;
    esac

    unset DRI_PRIME
    xrandr --setprovideroffloadsink $provider 0

    stop_xserver
}

check_driver_loaded() {
    cards=(`echo -n /sys/class/drm/card?`)
    if [[ ${#cards[@]} -lt 2 ]]; then
        return 1
    fi

    for card in /sys/class/drm/card?; do 
        if [[ ! -e $card/device/driver ]]; then
            return 1
        fi
    done

    return 0
}

# since we do not allow tow Xorg to be running simultaneously, 
# we assume DISPLAY will always be :0 if success
start_xserver() {
    if [[ x$opt_dryrun == x1 ]]; then
        export DISPLAY=:0
        return 0
    fi

    if [[ -z $HOME ]]; then
        export HOME=/root
    fi

    if [[ -z $USER ]]; then
        export USER=root
    fi

    pid=`pgrep -n -u $USER Xorg`
    if [[ -z $pid ]]; then
        xinit &
    fi

    export DISPLAY=:0
    sleep .1
}

stop_xserver() {
    unset DISPLAY

    if [[ x$opt_dryrun == x1 ]]; then
        return 0
    fi

    pid=`pgrep -n -u $USER Xorg`
    if [[ -n $pid ]]; then
        kill $pid
    fi

    sleep .1
}

usage() {
    echo "$0 [-n dryrun] [-h]" >&2
    exit 1
}

while getopts hn opt; do
    case $opt in
        n) opt_dryrun=1
            ;;
        h)  usage
            ;;
    esac
done

if [[ x$opt_dryrun != x1 && ! has_dual_cards ]]; then 
    msg "no dual cards setup, just quit"
    exit 0;
fi

# run X to get info from and then quit for doing testing
start_xserver

if [[ x$opt_dryrun != x1 && ! check_driver_loaded ]]; then
    bad "at least one graphic card has no driver"
    stop_xserver
    exit 1
fi

parse_cards_info

#FIXME: I found that when install nvidia proprietary driver, data under 
# /sys/class/drm/cardX is not adequte for parsing connectors related info

printf "     name\tstatus\tbus\tboot\tdrv\tdrmid\tsrcoff\tsinkoff\n"
printf "DIS: %s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n"  \
    $DIS_NAME $DIS_STATUS $DIS_BUS $DIS_BOOT $DIS_DRIVER $DIS_DRMID $DIS_SRC_OFFLOAD $DIS_SINK_OFFLOAD
printf "IGD: %s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
    $IGD_NAME $IGD_STATUS $IGD_BUS $IGD_BOOT $IGD_DRIVER $IGD_DRMID $IGD_SRC_OFFLOAD $IGD_SINK_OFFLOAD

stop_xserver

test_switch

test_offload_rendering 


