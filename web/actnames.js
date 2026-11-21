/* action codes -> names: shared by the remapper (remap.html) and the overlay (overlay.html) */
(function(){
  const KEYS={4:"A",5:"B",6:"C",7:"D",8:"E",9:"F",10:"G",11:"H",12:"I",13:"J",14:"K",15:"L",16:"M",17:"N",18:"O",19:"P",20:"Q",21:"R",22:"S",23:"T",24:"U",25:"V",26:"W",27:"X",28:"Y",29:"Z",
 30:"1",31:"2",32:"3",33:"4",34:"5",35:"6",36:"7",37:"8",38:"9",39:"0",40:"Enter",41:"Esc",42:"Backspace",43:"Tab",44:"Space",45:"- _",46:"= +",47:"[ {",48:"] }",49:"\\ |",51:"; :",52:"' \"",53:"` ~",54:", <",55:". >",56:"/ ?",57:"Caps Lock",
 58:"F1",59:"F2",60:"F3",61:"F4",62:"F5",63:"F6",64:"F7",65:"F8",66:"F9",67:"F10",68:"F11",69:"F12",70:"Print Screen",71:"Scroll Lock",72:"Pause",73:"Insert",74:"Home",75:"Page Up",76:"Delete",77:"End",78:"Page Down",79:"Right arrow",80:"Left arrow",81:"Down arrow",82:"Up arrow",
 83:"Num Lock",84:"Num /",85:"Num *",86:"Num -",87:"Num +",88:"Num Enter",89:"Num 1",90:"Num 2",91:"Num 3",92:"Num 4",93:"Num 5",94:"Num 6",95:"Num 7",96:"Num 8",97:"Num 9",98:"Num 0",99:"Num .",101:"Menu key"};
  const MODS={0xE0:"Left Ctrl",0xE1:"Left Shift",0xE2:"Left Alt",0xE3:"Left Win",0xE4:"Right Ctrl",0xE5:"Right Shift",0xE6:"Right Alt",0xE7:"Right Win"};
  const MOUSE={0x70:"Mouse left",0x71:"Mouse right",0x7A:"Mouse middle",0x7B:"Mouse back (4)",0x7C:"Mouse forward (5)",0x76:"Mouse move up",0x77:"Mouse move down",0x78:"Mouse move left",0x79:"Mouse move right",0x72:"Wheel up",0x73:"Wheel down",0x74:"Wheel left",0x75:"Wheel right"};
  const PADB=[[0,"A"],[1,"B"],[2,"X"],[3,"Y"],[19,"LB"],[9,"RB"],[27,"LT click"],[23,"RT click"],[17,"L4"],[18,"L5"],[7,"R4"],[8,"R5"],[13,"D-pad up"],[10,"D-pad down"],[12,"D-pad left"],[11,"D-pad right"],[6,"Menu"],[14,"View"],[4,"QAM"],[15,"L3"],[5,"R3"],[26,"Left pad click"],[22,"Right pad click"]];
  const XBOXB=[[16,"Guide"],[0,"A"],[1,"B"],[2,"X"],[3,"Y"],[19,"LB"],[9,"RB"],[27,"LT (full)"],[23,"RT (full)"],[6,"Start"],[14,"Back"],[15,"L3"],[5,"R3"],[13,"D-pad up"],[10,"D-pad down"],[12,"D-pad left"],[11,"D-pad right"]];
  const XTRIG=["Xbox LT (whole trigger, analog)","Xbox RT (whole trigger, analog)"];
  const STICKOUT=["Left stick up (full)","Left stick down (full)","Left stick left (full)","Left stick right (full)","Right stick up (full)","Right stick down (full)","Right stick left (full)","Right stick right (full)","Left trigger (full, analog)","Right trigger (full, analog)"];
  function name(a){ a=a|0; if(!a) return "";
    if(KEYS[a]) return KEYS[a]; if(MODS[a]) return MODS[a]; if(MOUSE[a]) return MOUSE[a];
    if(a>=0x80&&a<0xA0){ const b=PADB.find(x=>x[0]===a-0x80); return b?"Pad "+b[1]:"Pad button "+(a-0x80); }
    if(a>=0xA0&&a<0xC0){ const b=XBOXB.find(x=>x[0]===a-0xA0); return b?"Xbox "+b[1]:"Xbox button "+(a-0xA0); }
    if(a>=0xC0&&a<0xC4) return "Profile "+(a-0xBF); if(a>=0xC4&&a<0xC8) return "Layer: profile "+(a-0xC3);
    if(a>=0xC8&&a<0xCC) return "Macro "+(a-0xC7); if(a>=0xD0&&a<0xDA) return STICKOUT[a-0xD0]; if(a===0xDA||a===0xDB) return XTRIG[a-0xDA];
    return "0x"+a.toString(16); }
  /* short names for small labels (the overlay's menus) */
  const SHORT={0x70:"LMB",0x71:"RMB",0x7A:"MMB",0x7B:"Mouse 4",0x7C:"Mouse 5",0x72:"Wheel ↑",0x73:"Wheel ↓",0x74:"Wheel ←",0x75:"Wheel →",0x76:"Mouse ↑",0x77:"Mouse ↓",0x78:"Mouse ←",0x79:"Mouse →",
    0xE0:"Ctrl",0xE1:"Shift",0xE2:"Alt",0xE3:"Win",0xE4:"R Ctrl",0xE5:"R Shift",0xE6:"R Alt",0xE7:"R Win",79:"→",80:"←",81:"↓",82:"↑",41:"Esc",42:"Bksp",57:"Caps",76:"Del",75:"PgUp",78:"PgDn",
    45:"-",46:"=",47:"[",48:"]",49:"\\",51:";",52:"'",53:"`",54:",",55:".",56:"/"};
  function short(a){ a=a|0; if(SHORT[a]) return SHORT[a]; if(a>=0xC4&&a<0xC8) return "Layer "+(a-0xC3); if(a>=0xD0&&a<0xD8) return ["LS","LS","LS","LS","RS","RS","RS","RS"][a-0xD0]+" "+["↑","↓","←","→"][(a-0xD0)%4];
    if(a===0xD8) return "LT"; if(a===0xD9) return "RT"; if(a===0xDA) return "Xbox LT"; if(a===0xDB) return "Xbox RT"; return name(a); }
  window.SCA={KEYS,MODS,MOUSE,PADB,XBOXB,XTRIG,STICKOUT,name,short};
})();
