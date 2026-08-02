#!/bin/sh
# setup_configs.sh -- idempotently create the extio_test project in
# configs.db: one config per variant x box (subject = box name so datafiles
# carry the DUT identity) plus a certification queue per box.
#
#   ./setup_configs.sh [box ...]     default boxes: vbox box01
#
# Everything runs in the live dserv's `configs` subprocess via dservctl.

set -u
BOXES="${*:-vbox box01}"

# NB: `dservctl configs ...` is dservctl's own registry subcommand -- route
# through the main interp's `send` to reach the configs subprocess interp
cfg() { dservctl -c "send configs {$1}" 2>&1; }

echo "== project extio_test =="
r=$(cfg 'project_exists extio_test')
if [ "$r" = "1" ]; then
    echo "project exists"
else
    cfg 'project_create extio_test -description "extio platform end-to-end certification (loopback self-test)"'
fi
cfg 'project_activate extio_test' >/dev/null

for box in $BOXES; do
    echo "== configs for box $box =="
    for variant in wiring_check events timing analog amplitude mixed_soak; do
        name="${box}-${variant}"
        r=$(cfg "config_exists {$name}")
        if [ "$r" = "1" ]; then
            echo "  $name exists"
            continue
        fi
        cfg "config_create {$name} extio_test loopback $variant \
                -subject {$box} \
                -params {box $box} \
                -file_template {{subject}_{variant}_{date_short}{time}} \
                -description {extio certification: $variant on $box}" >/dev/null
        echo "  created $name"
    done

    qname="certify-$box"
    case "$(cfg 'queue_list')" in
        *"$qname"*) echo "  queue $qname exists" ;;
        *)
            cfg "queue_create {$qname} -description {full certification pass on $box}" >/dev/null
            for variant in wiring_check events timing analog amplitude mixed_soak; do
                cfg "queue_add_item {$qname} {${box}-${variant}}" >/dev/null
            done
            echo "  created queue $qname (5 items)"
            ;;
    esac
done

echo "== done =="
cfg 'config_list' | head -20
