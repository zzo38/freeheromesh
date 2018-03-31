"use strict";
const fs=require("fs");
const quarks=fs.readFileSync("quarks","ascii").split("\n").map(x=>x.trim()).filter(x=>x&&x[0]!="!");
quarks.forEach((x,y)=>console.log("#define Q_"+x+" "+(y+2)));
console.log("static const char*const global_quarks[]={");
quarks.forEach(x=>console.log("  \""+x+"\","));
console.log("0}; static const SDLKey quark_to_key[Q_undo+1-Q_backspace]={");
quarks.slice(quarks.indexOf("backspace"),quarks.indexOf("undo")+1).forEach(x=>console.log("SDLK_"+x[x.length==1?"toLowerCase":"toUpperCase"]()+","));
console.log("};\n#define FirstKeyQuark Q_backspace\n#define LastKeyQuark Q_undo");
