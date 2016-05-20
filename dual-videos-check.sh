#!/usr/bin/env bash
# check dual graphics video card setup
# this test works when DISPLAY is set and need sudo (without passwd) to 
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
    sudo bash -c '[[ -e /sys/kernel/debug/vgaswitcheroo/switch ]]'
}

has_optimus() {
    false
}

# parse /sys/kernel/debug/vgaswitcheroo/switch and assign to global args
parse_vgaswitch_file() {
    local switch_file oldifs 

    switch_file=$(mktemp -t video_test_XXXXXX)
    sudo cat /sys/kernel/debug/vgaswitcheroo/switch > $switch_file

    #FIXME: remove it, test only
    cat > $switch_file <<EOF
0:DIS: :DynOff:0000:01:00.0
1:IGD:+:Pwr:0000:00:02.0
EOF

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

    rm $switch_file
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
    } ' <(DISPLAY=:0 xrandr --listproviders)
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
    conn=( `awk '
        $0 ~ /connected/ {
        n = patsplit($1, a, /[a-zA-Z]+|[0-9]+/)
        for (i = 0; i <= n; i++)
            print a[i]
            exit 
        }' <(DISPLAY=:0 xrandr)` )

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

parse_cards_info() {
    if has_vgaswitcheroo; then
        parse_vgaswitch_file
    elif has_optimus; then
        parse_optimus
    fi

    # this is as default fallback
    parse_sysfs_for_info

    parse_provider_by_outputs
}

offloding_capable() {
    false
}

test_offload_rendering() {
    [[ ! offloding_capable ]] && return 0
}

test_switch_vgaswitcheroo() {
    #TODO: switch
    #TODO: check offload rendering
    :
}

test_switch_bumblebee() {
    :
}

test_switch() {
    if has_vgaswitcheroo; then
        test_switch_vgaswitcheroo
    elif has_optimus; then
        test_switch_bumblebee
    fi
}

check_driver_loaded() {
    for card in /sys/class/drm/card?; do 
        if [[ ! -e $card/device/driver ]]; then
            return 1
        fi
    done

    return 0
}

start_xserver() {
    :
}

stop_xserver() {
    :
}

# run X to get info from and then quit for doing testing
start_xserver

if ! has_dual_cards; then 
    msg "no dual cards setup, just quit"
    exit 0;
fi

if ! check_driver_loaded; then
    bad "at least one graphic card has no driver"
    exit 1
fi

parse_cards_info

#FIXME: I found that when install nvidia proprietary driver, data under 
# /sys/class/drm/cardX is not adequte for parsing connectors related info

printf "     name\tsta\tbus\tboot\tdrv\tdrm\n"
printf "DIS: %s\t%s\t%s\t%s\t%s\t%s\n"  \
    $DIS_NAME $DIS_STATUS $DIS_BUS $DIS_BOOT $DIS_DRIVER $DIS_DRMID
printf "IGD: %s\t%s\t%s\t%s\t%s\t%s\n" \
    $IGD_NAME $IGD_STATUS $IGD_BUS $IGD_BOOT $IGD_DRIVER $IGD_DRMID

stop_xserver

test_switch

test_offload_rendering 

