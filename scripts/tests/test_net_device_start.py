#!/usr/bin/env python3
"""Exercise native NIC activation, failed-start fallback, and interface naming."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/src/net/core.c').read_text()


def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


test = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "net/core.h"
#define klogf(...) ((void)printf(__VA_ARGS__))
static bool ready=true;
static struct net_device *devices[NET_DEVICE_MAX], *default_device;
static int starts, stops;
static bool start(struct net_device *dev) {starts++;return dev->driver_private!=NULL;}
static bool link_up(struct net_device *dev) {(void)dev;return true;}
static void stop(struct net_device *dev) {(void)dev;stops++;}
'''
for signature in ['int net_device_register(', 'bool net_devices_start(',
                  'int net_device_unregister(', 'struct net_device *net_device_default(',
                  'struct net_device *net_device_find(', 'bool net_device_set_default(',
                  'static bool packet_current(']:
    test += function(signature) + '\n'
test += r'''
int main(void) {
 const struct net_device_ops ops={.start=start,.link_up=link_up,.stop=stop};
 const struct net_device_ops active={.link_up=link_up,.stop=stop};
 struct net_device bad={.ops=&ops},good={.ops=&ops,.driver_private=(void*)1};
 assert(!net_devices_start() && !starts);
 assert(!net_device_register(&bad) && !strcmp(bad.name,"eth0"));
 assert(!net_device_register(&good) && !strcmp(good.name,"eth1"));
 struct net_packet packet={.device=&good,.generation=good.generation};
 assert(packet_current(&packet));
 good.generation++;assert(!packet_current(&packet));
 packet.generation=good.generation;assert(packet_current(&packet));
 assert(!net_device_set_default("eth1")); /* activation must finish first */
 assert(net_devices_start() && starts==2 && stops==1);
 assert(!bad.registered && !bad.started && good.started);
 assert(net_device_default()==&good && !net_device_find("eth0"));
 assert(net_devices_start() && starts==2 && stops==1);
 struct net_device live={.ops=&active};
 assert(!net_device_register(&live) && !strcmp(live.name,"eth0"));
 assert(net_devices_start() && live.started && starts==2);
 assert(net_device_set_default("eth1") && net_device_default()==&good);
 assert(!net_device_set_default("eth7") && net_device_default()==&good);
 struct net_device duplicate={.ops=&active,.name="eth0"};
 assert(net_device_register(&duplicate)==-1);
 assert(!net_device_unregister(&live) && !live.started);
 assert(net_device_default()==&good);
 assert(!net_device_unregister(&good) && !net_devices_start());
 assert(!packet_current(&packet));
 puts("PASS: empty boot, automatic names, collisions, failed-start fallback, optional start, repeated startup");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-net-start-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(test)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT / 'kernel/src'), str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
