"use strict";
const fs=require("fs");
const quarks=fs.readFileSync("quarks","ascii").split("\n").map(x=>x.trim()).filter(x=>x&&x[0]!="!");
quarks.forEach((x,y)=>console.log("#define Q_"+x+" "+(y+2)));
console.log("static const char*const global_quarks[]={");
quarks.forEach(x=>console.log("  \""+x+"\","));
console.log("0};");
