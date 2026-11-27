// SC2 Overlay: the Steam Input layout download (steamConfig + the 127.0.0.1 server) with Steam stubbed
const fs=require('fs'); const src=fs.readFileSync(__dirname+'/../../desktop/main.js','utf8');
const a=src.indexOf('const STEAMCFG_PORT'), b=src.indexOf('app.whenReady().then(() => {\n  protocol.handle');
const vdfFile=fs.readdirSync('/mnt/user-data/uploads').find(f=>f.endsWith('.vdf')); const VDF=fs.readFileSync('/mnt/user-data/uploads/'+vdfFile,'utf8');
let api={response:{publishedfiledetails:[{result:1,consumer_app_id:241100,filename:'3843516937_controller_config.vdf',file_size:'23223',file_url:'https://cdn.steamusercontent.com/ugc/1/A/',title:'Tiny Tina Steam Controller'}]}};
const calls=[];
const net={ fetch: async (u,o)=>{ calls.push([u,o&&o.method,o&&o.body]); if(u.includes('GetPublishedFileDetails')) return {ok:true,status:200,json:async()=>api}; return {ok:true,status:200,text:async()=>VDF}; } };
const ORIGIN='app://local';
eval(src.slice(a,b).replace('const STEAMCFG_PORT = 27150','const STEAMCFG_PORT = 27151'));
(async()=>{
  const ok=(n,c,i='')=>console.log((c?'PASS ':'FAIL ')+n+(c?'':' '+i));
  const r=await steamConfig('3843516937'); ok('downloads', r.title==='Tiny Tina Steam Controller' && r.vdf===VDF);
  ok('POST with the id', calls[0][1]==='POST' && /publishedfileids%5B0%5D=3843516937/.test(calls[0][2]), JSON.stringify(calls[0]));
  for(const bad of ['abc','12','1;rm']) { try{ await steamConfig(bad); ok('rejects '+bad,false); }catch(e){ ok('rejects '+bad, /number/.test(e.message)); } }
  api.response.publishedfiledetails[0].consumer_app_id=440; try{ await steamConfig('3843516937'); ok('not a layout',false);}catch(e){ ok('not a layout', /not a Steam Input/.test(e.message)); }
  api.response.publishedfiledetails[0].consumer_app_id=241100; api.response.publishedfiledetails[0].file_url='https://evil.example.com/x';
  try{ await steamConfig('3843516937'); ok('other host',false);}catch(e){ ok('other host refused', /unexpected/.test(e.message)); }
  api.response.publishedfiledetails[0].file_url='https://cdn.steamusercontent.com.evil.com/x';
  try{ await steamConfig('3843516937'); ok('look-alike host',false);}catch(e){ ok('look-alike host refused', /unexpected/.test(e.message)); }
  api.response.publishedfiledetails[0].file_url='https://cdn.steamusercontent.com/ugc/1/A/'; api.response.publishedfiledetails[0].result=9;
  try{ await steamConfig('3843516937'); ok('missing',false);}catch(e){ ok('missing item', /no layout/.test(e.message)); }
  api.response.publishedfiledetails[0].result=1;
  steamConfigServer(); await new Promise(r=>setTimeout(r,200));
  const http=require('http'); const get=(path,origin,method='GET')=>new Promise(res=>{ const q=http.request({host:'127.0.0.1',port:27151,path,method,headers:origin?{Origin:origin,'Access-Control-Request-Private-Network':'true'}:{}},r=>{let d='';r.on('data',c=>d+=c);r.on('end',()=>res({s:r.statusCode,h:r.headers,d}));}); q.end(); });
  let x=await get('/steam-config?id=3843516937','https://pablo89898991.github.io'); ok('server: page origin gets it', x.s===200 && JSON.parse(x.d).vdf===VDF && x.h['access-control-allow-origin']==='https://pablo89898991.github.io' && x.h['access-control-allow-private-network']==='true', x.s+' '+JSON.stringify(x.h));
  x=await get('/steam-config?id=3843516937','https://evil.example.com'); ok('server: other origin refused', x.s===403 && !x.h['access-control-allow-origin'], x.s);
  x=await get('/steam-config?id=3843516937',null); ok('server: no origin refused', x.s===403, x.s);
  x=await get('/steam-config?id=3843516937','https://pablo89898991.github.io','OPTIONS'); ok('server: preflight', x.s===204 && x.h['access-control-allow-private-network']==='true', x.s);
  x=await get('/other','https://pablo89898991.github.io'); ok('server: other path 404', x.s===404);
  x=await get('/steam-config?id=zz','https://pablo89898991.github.io'); ok('server: bad id 400', x.s===400 && /number/.test(JSON.parse(x.d).error));
  process.exit(0);
})();
