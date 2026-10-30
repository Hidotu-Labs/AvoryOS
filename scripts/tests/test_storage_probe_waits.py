#!/usr/bin/env python3
"""Fault-inject absent ATA buses and stuck AHCI DMA engines."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
def function(source, signature):
    start=source.index(signature); end=source.index('{',start)+1; depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]+'\n'
ahci=(ROOT/'kernel/src/drivers/storage/ahci.c').read_text()
ata=(ROOT/'kernel/src/drivers/storage/ata.c').read_text()
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "drivers/storage/ahci.h"
#define ATA_REG_STATUS 7
#define ATA_SR_BSY 0x80
#define ATA_SR_DRQ 8
#define ATA_SR_ERR 1
#define ATA_SR_DF 0x20
static uint64_t now;
static unsigned reads, scenario;
static uint8_t status;
static ahci_port_t *active;
static uint64_t lapic_timer_get_ms(void){return now++;}
static uint8_t inb(uint16_t port){(void)port;reads++;return status;}
static void hal_cpu_relax(void){
 if(scenario==1) {
  if(active->cmd & AHCI_CMD_CR) {
   assert(active->cmd & AHCI_CMD_FRE);active->cmd &= ~AHCI_CMD_CR;
  } else active->cmd &= ~AHCI_CMD_FR;
 }
}
'''
code+=function(ahci,'static bool quiesce_port(')
code+=function(ata,'static int ata_wait_bsy(')
code+=function(ata,'static int ata_wait_drq(')
code+=r'''
int main(void){
 ahci_port_t port={0};active=&port;
 scenario=1;port.cmd=AHCI_CMD_ST|AHCI_CMD_CR|AHCI_CMD_FRE|AHCI_CMD_FR;
 assert(quiesce_port(&port));assert(!(port.cmd&(AHCI_CMD_ST|AHCI_CMD_CR|AHCI_CMD_FRE|AHCI_CMD_FR)));
 scenario=0;now=0;port.cmd=AHCI_CMD_ST|AHCI_CMD_CR|AHCI_CMD_FRE;
 assert(!quiesce_port(&port));assert(now<=1002);assert(port.cmd&AHCI_CMD_FRE);
 now=0;port.cmd=AHCI_CMD_FRE|AHCI_CMD_FR;assert(!quiesce_port(&port));assert(now<=1003);
 for(unsigned i=0;i<2;i++) {
  status=i?0xff:0;reads=0;assert(ata_wait_bsy(0x1f0)==-1);assert(reads==1);
  reads=0;assert(ata_wait_drq(0x1f0)==-1);assert(reads==1);
 }
 status=0x40;assert(!ata_wait_bsy(0x1f0));
 status=ATA_SR_DRQ;assert(!ata_wait_drq(0x1f0));
 status=ATA_SR_ERR;assert(ata_wait_drq(0x1f0)==-1);
 status=ATA_SR_BSY;reads=0;assert(ata_wait_bsy(0x1f0)==-1);assert(reads==100000);
}
'''
with tempfile.TemporaryDirectory(prefix='storage-waits-') as directory:
    p=Path(directory);(p/'test.c').write_text(code)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-I',str(ROOT/'kernel/src'),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS: absent ATA exits after one read; stuck AHCI engines time out; stop ordering and ready/error paths preserved')
