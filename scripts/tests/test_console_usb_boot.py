#!/usr/bin/env python3
"""USB Enter must complete a console line and present output without more input."""
import json
import socket
import time
from boot_probe import ROOT, run_probe

INIT = r'''
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
int main(void) {
    setbuf(stdout, NULL);
    int tty = open("/dev/tty1", O_RDWR);
    dup2(tty, 0); dup2(tty, 1); dup2(tty, 2);
    struct termios t;
    tcgetattr(0, &t); t.c_lflag |= ICANON | ECHO; t.c_iflag |= ICRNL;
    tcsetattr(0, TCSANOW, &t);
    int card = open("/dev/dri/card0", O_RDWR); if (card >= 0) close(card);
    puts("CONSOLE_USB_BOOT: START");
    printf("\033[2J\033[H\033[?25lINPUT_READY\n");
    char buffer[64]; int n = read(0, buffer, sizeof(buffer));
    printf("READ_DONE n=%d\n", n);
    printf("\033[2J\033[H\033[42m                                                                \033[0m");
    t.c_lflag &= ~ECHO; tcsetattr(0, TCSANOW, &t);
    printf("\033[2;1HPASSWORD_READY\n");
    n = read(0, buffer, sizeof(buffer));
    printf("PASSWORD_DONE n=%d\n", n);
    printf("\033[2J\033[H\033[44m                                                                \033[0m");
    /* This final frame must also appear while another read waits. */
    read(0, buffer, sizeof(buffer));
    for (;;) sleep(1);
}
'''


def main():
    from PIL import Image
    out = ROOT/'build/console-usb-boot'
    out.mkdir(parents=True, exist_ok=True)
    qmp = out/'qmp.sock'
    qmp.unlink(missing_ok=True)
    (out/'serial.log').unlink(missing_ok=True)
    result = {}
    def started(process, output):
        serial = output/'serial.log'
        def wait_text(text, seconds=30):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline and process.poll() is None:
                if serial.exists() and text in serial.read_text(errors='replace'):
                    return True
                time.sleep(.02)
            return False
        if not wait_text('INPUT_READY'):
            raise RuntimeError('console input prompt did not start')
        with socket.socket(socket.AF_UNIX) as conn:
            conn.connect(str(qmp)); conn.settimeout(10)
            stream = conn.makefile('rwb', buffering=0)
            json.loads(stream.readline())
            def command(name, arguments=None):
                stream.write(json.dumps({'execute': name, 'arguments': arguments or {}}).encode() + b'\n')
                while True:
                    reply = json.loads(stream.readline())
                    if 'error' in reply: raise RuntimeError(reply)
                    if 'return' in reply: return reply['return']
            command('qmp_capabilities')
            def key(code):
                event = {'type':'key', 'data':{'down':True, 'key':{'type':'qcode','data':code}}}
                command('input-send-event', {'events':[event]})
                time.sleep(.05)
                event['data']['down'] = False
                command('input-send-event', {'events':[event]})
                time.sleep(.05)
            for code in ('r','o','o','t','ret'):
                key(code)
            result['read_completed_without_extra_event'] = wait_text('READ_DONE n=5', 3)
            time.sleep(.5)
            before = output/'before-extra-event.ppm'
            command('screendump', {'filename':str(before)})
            pixel = Image.open(before).convert('RGB').getpixel((8,8))
            result['output_presented_without_extra_event'] = pixel[1] > pixel[0] + 30 and pixel[1] > pixel[2] + 30
            result['username_pixel'] = pixel
            for code in ('r','o','o','t','ret'):
                key(code)
            result['password_read_completed_without_extra_event'] = wait_text('PASSWORD_DONE n=5', 3)
            time.sleep(.5)
            after = output/'password-after-enter.ppm'
            command('screendump', {'filename':str(after)})
            pixel = Image.open(after).convert('RGB').getpixel((8,8))
            result['password_output_presented_without_extra_event'] = pixel[2] > pixel[0] + 30 and pixel[2] > pixel[1] + 30
            result['password_pixel'] = pixel
            command('quit')
    run_probe('console-usb-boot', INIT, 'READ_DONE n=5', cpus=8, memory='4G',
              started=started, timeout=50,
              qemu_args=['-device','qemu-xhci','-device','usb-kbd','-device','usb-mouse',
                         '-qmp',f'unix:{qmp},server=on,wait=off'])
    (out/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(result)
    if any(not result[key] for key in ('read_completed_without_extra_event',
            'output_presented_without_extra_event', 'password_read_completed_without_extra_event',
            'password_output_presented_without_extra_event')):
        raise RuntimeError('Console Enter/output needs an extra input event')

if __name__ == '__main__':
    main()
