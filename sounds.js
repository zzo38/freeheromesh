// This program will read the sounds from HEROFALL.EXE
// The output is written to a Hamster archive to stdout
"use strict";
const hamarc=require("hamarc");
const fs=require("fs");
const out=new hamarc.File(1);
const hdr=Buffer.allocUnsafe(12);
const fil=fs.openSync(process.argv[2],"r");
const names=`
 SPLASH
 POUR
 DOOR
 GLASS
 BANG
 UNHH
 UH_OH
 FROG
 THWIT
 KLINKK
 POWER
 KLECK
 CLICK
 SMALL_POP
 DINK
 TICK
 CHYEW
 CHEEP
 ANHH
 BRRRT
 BRRREEET
 BWEEP
 DRLRLRINK
 FFFFTT
 WAHOO
 YEEHAW
 OLDPHONE
 RATTLE
 BEEDEEP
 THMP_thmp
 BEDOINGNG
 HEARTBEAT
 LOCK
 TAHTASHH
 BOOOM
 VACUUM
 RATCHET2
 DYUPE
 UNCORK
 BOUNCE
 JAYAYAYNG
 DEEP_POP
 RATCHET1
 GLISSANT
 BUZZER
 FAROUT
 KEWEL
 WHACK
 STEAM
 HAWK
`.split("\n").map(x=>x.trim()).filter(x=>x);
let n=0;
let o=process.argv[3]?parseInt(process.argv[3],16):0x5CD92;
for(;;) {
  if(fs.readSync(fil,hdr,0,12,o)<12) break;
  if(hdr.toString("ascii",0,4)!="RIFF" || hdr.toString("ascii",8,12)!="WAVE") break;
  let len=hdr.readUInt32LE(4);
  let buf=Buffer.allocUnsafeSlow(len+8);
  fs.readSync(fil,buf,0,len,o);
  out.put(names[n]+".WAV",buf);
  o+=len+8;
  n++;
}
