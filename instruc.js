"use strict";
const fs=require("fs");
const names_file=fs.readFileSync("names.js","ascii").split("\n");
const data_file=fs.readFileSync("instruc","ascii").split("\n");
const do_sound_names=x=>{
  if(!x || x[0]=="`") return;
  if(x[0]=="c") return f=()=>0;
  let y=/^ *([A-Za-z_0-9]+) *$/.exec(x);
  if(y) return data_file.push("#"+y[1]);
};
const do_message_names=x=>{
  if(x.startsWith("const standard_sound_names=")) {
    data_file.push("(0300)");
    return f=do_sound_names;
  }
  let y=/^ *([0-9]+) = ([^ ]*) *$/.exec(x);
  if(y) data_file.push("#"+y[2]+" ("+(Number(y[1])+0x0200).toString(16)+")");
};
let f=x=>{
  if(x.startsWith("const standard_message_names=")) f=do_message_names;
};
names_file.forEach(x=>f(x)); // not .forEach(f); the function to use varies
let curnum=0;
const names=Object.create(null);
data_file.forEach((line,linenum)=>{
  const reg=/^[ \t]*([-,=!+*#.]*)[ \t]*([A-Za-z0-9_]+)?[ \t]*(?:"([^" \t]+)")?[ \t]*(?:\(([0-9A-Fa-f]+)\))?[ \t]*(?:;.*)?$/.exec(line);
  if(!reg) throw "Syntax error on line "+(linenum+1);
  let flags=128;
  if(reg[1].includes(",")) flags|=1;
  if(reg[1].includes("=")) flags|=2;
  if(reg[1].includes("-")) flags|=4;
  if(reg[1].includes("!")) flags|=8;
  if(reg[1].includes("+")) flags|=16;
  if(reg[1].includes(".")) flags|=32;
  if(reg[4]) curnum=parseInt(reg[4],16);
  if(reg[2] && !reg[1].includes("#")) {
    console.log("#define OP_"+reg[2].toUpperCase()+" "+curnum);
    if(flags&1) console.log("#define OP_"+reg[2].toUpperCase()+"_C "+(curnum+0x0800));
    if(flags&2) console.log("#define OP_"+reg[2].toUpperCase()+"_E "+(curnum+0x1000));
    if(3==(flags&3)) console.log("#define OP_"+reg[2].toUpperCase()+"_EC "+(curnum+0x1800));
    if(10==(flags&10)) {
      if(flags&1) console.log("#define OP_"+reg[2].toUpperCase()+"_EC16 "+(curnum+0x0801));
      console.log("#define OP_"+reg[2].toUpperCase()+"_E16 "+(curnum+0x1001));
    }
    if(flags&32) {
      console.log("#define OP_"+reg[2].toUpperCase()+"_D "+(curnum+0x2000));
      if(flags&1) console.log("#define OP_"+reg[2].toUpperCase()+"_CD "+(curnum+0x2800));
    }
  }
  if(reg[2] && !reg[1].includes("*")) names[reg[3]||reg[2]]=curnum|(flags<<16);
  if(reg[2]) ++curnum;
  if(flags&8) ++curnum;
});
console.log("#ifdef HEROMESH_CLASS\nstatic const Op_Names op_names[]={");
Object.keys(names).sort().forEach(x=>console.log("{\""+x+"\","+names[x]+"},"));
console.log("};\n#define N_OP_NAMES "+Object.keys(names).length+"\n#endif");
