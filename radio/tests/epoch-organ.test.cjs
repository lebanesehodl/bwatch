// Pure score/lifecycle checks; real Web Audio renders are separate evidence.
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),vm=require('node:vm'),path=require('node:path'),{webcrypto}=require('node:crypto');
const html=fs.readFileSync(process.env.RADIO_SOURCE||path.join(__dirname,'../index.html'),'utf8');
const source=[...html.matchAll(/<script[^>]*>([\s\S]*?)<\/script>/g)].at(-1)[1];
function page(search=''){
  const elements=new Map(),clock={wall:1800000000000,mono:0},canvas=new Proxy({},{get:()=>()=>{}});
  class ClockDate extends Date{static now(){return clock.wall}}
  const el=id=>{if(!elements.has(id))elements.set(id,{textContent:id==='srcNote'?html.match(/<p class="note" id="srcNote">([^<]+)<\/p>/)[1]:'',style:{},classList:{add(){},remove(){}},addEventListener(){},setAttribute(){},querySelectorAll(){return []},getBoundingClientRect(){return {width:800}},getContext(){return canvas},closest(){return {hidden:false}}});return elements.get(id)};
  const env=vm.createContext({console,crypto:webcrypto,TextEncoder,Uint8Array,Float32Array,Date:ClockDate,performance:{now:()=>clock.mono},Math,Number,Promise,AbortController,setTimeout(){},clearTimeout(){},setInterval(){},clearInterval(){},navigator:{},location:{hostname:'localhost',search},requestAnimationFrame:fn=>fn(),getComputedStyle:()=>({getPropertyValue:()=>''}),document:{getElementById:el,querySelectorAll:()=>[],querySelector:()=>el('button')},window:{addEventListener(){},devicePixelRatio:1},speechSynthesis:{cancel(){}}});
  vm.runInContext(source,env);return {env,clock,run:s=>vm.runInContext(s,env),json:s=>JSON.parse(vm.runInContext('JSON.stringify('+s+')',env))};
}
function audio(p){p.run(`
  const oscillators=[],params=[];
  const param=()=>{const p={value:0,events:[],setValueAtTime(v,t){this.events.push(['set',v,t])},linearRampToValueAtTime(v,t){this.events.push(['ramp',v,t])},setTargetAtTime(v,t,d){this.events.push(['target',v,t,d])},cancelScheduledValues(t){this.events.push(['cancel',t])}};params.push(p);return p};
  const node=()=>({connect(){},gain:param(),frequency:param(),Q:param()});
  ctx={currentTime:100,state:'running',createGain:node,createBiquadFilter:node,createStereoPanner(){return {connect(){},pan:param()}},createOscillator(){const n={connect(){},frequency:param(),detune:param(),starts:[],stops:[],start(t){this.starts.push(t)},stop(t){this.stops.push(t)}};oscillators.push(n);return n}};bus=node();
  initAudio=unlockAudio=startBed=stopBed=trim=loadFor=draw=presenceOn=presenceOff=flash=()=>{};
  transmit=(h,by,t)=>t+12;grain=()=>{};
`)}
function network(p,height=979779){p.env.fixture=Array.from({length:10},(_,i)=>({height:height-i,timestamp:(p.clock.wall-(i+1)*600000)/1000,id:(height-i).toString(16).padStart(64,'0'),tx_count:3000}));p.run(`getJSON=async u=>u.endsWith('/v1/blocks')?fixture:u.endsWith('/mempool')?{count:12000}:{};`)}
test('the repertoire is height-derived, cumulative, and explicitly finite',()=>{
  const p=page();assert.equal(p.run('organPalette(979775).length'),8);assert.equal(p.run('organPalette(979776).length'),9);assert.equal(p.run('organPalette(981792).length'),10);
  assert.equal(p.run('organPalette(0).length'),8);assert.equal(p.run('organPalette(2016*10000).length'),24);
  for(let n=0;n<16;n++){const h=(486+n)*2016;assert.equal(p.run(`newOrganChord(${h})`),p.run(`ORGAN_ADDITIONS[${n}].name`));assert.equal(p.run(`newOrganChord(${h+1})`),null)}
  assert.equal(p.run('newOrganChord(502*2016)'),null,'exhaustion is not disguised as another new chord');
  assert.deepEqual(p.json('organPalette(979775)'),p.json('ORGAN_BASE_NAMES'),'visiting a future epoch does not leak unlocks into old replay');
});
test('each authored addition has a distinct pitch-class colour and three valid four-note voicings',()=>{
  const p=page(),data=p.json('({CH,ORGAN_VOICES,ORGAN_BASE_NAMES,ORGAN_ADDITIONS})');
  const signature=ch=>[...new Set([ch[0],...ch[1]].map(n=>n%12))].sort((a,b)=>a-b).join(',');
  const seen=new Set(data.ORGAN_BASE_NAMES.map(n=>signature(data.CH[n])));
  for(const a of data.ORGAN_ADDITIONS){assert.ok(!seen.has(signature(a.ch)),a.name+' is not a relabelled existing colour');seen.add(signature(a.ch));for(const name of [...a.from,...a.to])assert.ok(data.ORGAN_BASE_NAMES.includes(name),name)}
  for(const [name,voices] of Object.entries(data.ORGAN_VOICES)){
    const pcs=new Set([data.CH[name][0],...data.CH[name][1]].map(n=>n%12));assert.equal(voices.length,3,name);
    for(const notes of voices){assert.equal(notes.length,4);assert.equal(new Set(notes).size,4);assert.deepEqual(notes,notes.slice().sort((a,b)=>a-b));for(const n of notes){assert.ok(n>=54&&n<=81,name+' base register');assert.ok(pcs.has(n%12),name+' chord tone '+n)}}
  }
});
test('old epochs keep the exact original transition choices and locked chords never leak',()=>{
  const p=page();
  assert.equal(p.run(`ORGAN_BASE_NAMES.every(cur=>Array.from({length:256},(_,b)=>{const by=new Uint8Array(32);by[0]=b;return blockChord(cur,by,979775)===NEXT[cur][b%NEXT[cur].length]}).every(Boolean))`),true);
  for(const h of [0,979775,979777,981793,1010017,99999999]){
    p.env.h=h;
    assert.equal(p.run(`organPalette(h).every(cur=>Array.from({length:256},(_,b)=>{const by=new Uint8Array(32);by[0]=b;const next=blockChord(cur,by,h);return next!==cur&&organPalette(h).includes(next)}).every(Boolean))`),true,'allowed transitions at '+h);
  }
});
test('all earlier additions remain reachable through authored transitions as the score grows',()=>{
  const p=page();assert.equal(p.run(`Array.from({length:17},(_,n)=>{const h=(485+n)*2016+1,seen=new Set(['Dmaj9']),queue=['Dmaj9'];while(queue.length){const cur=queue.shift();for(let j=0;j<256;j++){const by=new Uint8Array(32);by[0]=j;const next=blockChord(cur,by,h);if(!seen.has(next)){seen.add(next);queue.push(next)}}}return organPalette(h).every(ch=>seen.has(ch))}).every(Boolean)`),true);
});
test('each debut is introduced regardless of the hash, but hidden stations retain their original selection',()=>{
  const p=page();assert.equal(p.run(`ORGAN_BASE_NAMES.every(c=>blockChord(c,new Uint8Array(32),979776)==='Gm6')`),true);
  p.run(`st='lofi'`);assert.equal(p.run(`blockChord('Dmaj9',new Uint8Array(32),979776)`),'Bm11');
  assert.ok(p.run(`nextChord('Gm6',new Uint8Array(32))`),'leaving Shortwave cannot throw on its expanded chord');
});
test('hash preferences favour mid/open and voice-leading chooses the minimum bounded movement score',()=>{
  const p=page();assert.deepEqual(p.json(`Array.from({length:8},(_,i)=>{const b=new Uint8Array(32);b[7]=i;return organVariant('Dmaj9',b,979777,null)})`),[0,0,0,0,1,1,1,2]);
  assert.equal(p.run(`Object.keys(ORGAN_VOICES).every(ch=>Array.from({length:8},(_,b)=>{const by=new Uint8Array(32);by[7]=b;const previous={chord:'Dmaj9',organ:1,height:979775},preferred=[0,0,0,0,1,1,1,2][b],old=organNotes(previous.chord,1,previous.height),scores=ORGAN_VOICES[ch].map((_,i)=>organNotes(ch,i,979777).reduce((s,n,j)=>s+Math.abs(n-old[j]),0)+(i===preferred?0:16)),chosen=organVariant(ch,by,979777,previous);return scores[chosen]===Math.min(...scores)&&chosen===organVariant(ch,by,979777,previous)}).every(Boolean))`),true);
});
test('every voicing keeps the existing epoch transposition and halving octave instead of folding it away',()=>{
  const p=page();assert.equal(p.run(`Object.keys(ORGAN_VOICES).every(ch=>[0,1,2].every(i=>organNotes(ch,i,1050000).every((n,j)=>n===organNotes(ch,i,1049999)[j]-12)))`),true);
  assert.deepEqual(p.json(`organNotes('Dmaj9',0,977760).map((n,j)=>n-organNotes('Dmaj9',0,977759)[j])`),[-5,-5,-5,-5]);
});
test('the production Shortwave pad has only four square voices with the old total squared gain budget',()=>{
  const p=page();audio(p);p.run(`setKeyFor(979777);let played=makePad('Gm6','shortwave',100,1)`);
  assert.equal(p.run('oscillators.length'),4);assert.equal(p.run(`oscillators.every(o=>o.type==='square')`),true);
  const gains=p.json('params.filter(p=>p.value>0&&p.value<0.01).map(p=>p.value)');assert.equal(gains.length,4);assert.ok(Math.abs(gains.reduce((s,n)=>s+n*n,0)-0.0003)<1e-12);
  assert.deepEqual(p.json('oscillators.map(o=>o.frequency.value)'),p.json(`organNotes('Gm6',1,979777).map(mtof)`));
  p.run('played.stop(110)');assert.equal(p.run('oscillators.every(o=>o.stops.includes(113.2))'),true);
});
test('recalling the same chord with its different remembered voicing actually replaces the pad',()=>{
  const p=page();audio(p);p.run(`curChord='Dmaj9';currentOrgan=0;pad=makePad(curChord,st,100,0);setChord('Dmaj9',110,979775,1)`);
  assert.equal(p.run('currentOrgan'),1);assert.equal(p.run('oscillators.length'),8);assert.equal(p.run('oscillators.slice(0,4).every(o=>o.stops.includes(113.2))'),true);
});
test('wait-noise is zero at zero wait, grows to the original ceiling, and the introduction does not alter backlog',()=>{
  const p=page();audio(p);p.run(`bed={st:'shortwave',gains:{hiss:node(),whistle:node(),whistleOsc:node()}};mempoolCount=12000;const rainBefore=rainRate();bedTick(0,rainBefore,100)`);
  assert.equal(p.run('bed.gains.hiss.gain.events.at(-1)[1]'),0);assert.equal(p.run('bed.gains.whistle.gain.events.at(-1)[1]'),0);
  p.run('bedTick(1,rainRate(),101)');assert.equal(p.run('bed.gains.hiss.gain.events.at(-1)[1]'),0.032);assert.equal(p.run('bed.gains.whistle.gain.events.at(-1)[1]'),0.004);
  p.run('organIntroUntil=120;bedTick(1,rainRate(),110)');assert.equal(p.run('bed.gains.hiss.gain.events.at(-1)[1]'),0);assert.equal(p.run('bed.gains.whistle.gain.events.at(-1)[1]'),0);assert.equal(p.run('rainRate()===rainBefore'),true);
});
test('an observed new block resets the wait without rewriting its miner timestamp or the initial join age',()=>{
  const p=page();p.env.record={height:979776,timestamp:(p.clock.wall-600000)/1000,tx_count:3000,id:'a'.repeat(64)};
  assert.equal(p.run('liveBlock(record).waitAge'),10);assert.equal(p.run('liveBlock(record,true).waitAge'),0);assert.equal(p.run('liveBlock(record,true).time'),p.clock.wall-600000);
  p.run('chain=[liveBlock(record,true)];lastIdx=0');p.clock.mono+=60000;assert.equal(p.run('waitMin()'),1);
});
test('the new chord stays exposed through transmission and eight seconds after it, then history can resume',async()=>{
  const p=page();audio(p);p.run(`playing=true;chain=[{height:979776,time:Date.now(),hash:'a'.repeat(64),tx:3000}];lastIdx=-1;`);await p.run('fireBlock(0,100)');
  assert.equal(p.run('curChord'),'Gm6');assert.equal(p.run('currentOrgan'),0);assert.ok(Math.abs(p.run('organIntroUntil-(swBusy-0.5)')-8)<1e-9);assert.ok(p.run('harm.next>organIntroUntil'));
  p.run(`let recalled=0;setChord=()=>recalled++;harm.next=0;harmonyTick(organIntroUntil-0.01,1)`);assert.equal(p.run('recalled'),0);p.run('harmonyTick(organIntroUntil+0.01,1)');assert.equal(p.run('recalled'),1);
});
test('a busy debut uses the actual old sign-off, not the extra broadcast guard, and queues no second transmission',async()=>{
  const p=page();audio(p);p.run(`playing=true;let broadcasts=0;transmit=(h,by,t)=>{broadcasts++;return t+12};chain=[{height:979775,time:Date.now(),hash:'a'.repeat(64),tx:3000},{height:979776,time:Date.now(),hash:'b'.repeat(64),tx:3000}];lastIdx=-1`);
  await p.run('fireBlock(0,100)');p.run('const signedOff=swBusy-0.5;ctx.currentTime=signedOff+0.25');await p.run('fireBlock(1,ctx.currentTime)');
  assert.equal(p.run('curChord'),'Gm6');assert.equal(p.run('broadcasts'),1);assert.ok(Math.abs(p.run('organIntroUntil-signedOff')-8)<1e-9);assert.ok(p.run('organIntroUntil>ctx.currentTime'));
});
test('newer blocks and Stop clear the old introduction instead of letting it leak into another run',async()=>{
  const p=page();audio(p);p.run(`playing=true;chain=[{height:979776,time:Date.now(),hash:'a'.repeat(64),tx:3000},{height:979777,time:Date.now(),hash:'b'.repeat(64),tx:3000}];lastIdx=-1`);
  await p.run('fireBlock(0,100)');assert.ok(p.run('organIntroUntil>0'));await p.run('fireBlock(1,101)');assert.equal(p.run('organIntroUntil'),0);
  p.run('organIntroUntil=200;stop()');assert.equal(p.run('organIntroUntil'),0);assert.equal(p.run('swBusy'),0);
});
test('an older replay digest cannot introduce an old chord after a newer block in the same run',async()=>{
  const p=page();audio(p);p.run(`playing=true;chain=[{height:979776,time:Date.now(),hash:'old',tx:3000},{height:979777,time:Date.now(),hash:'new',tx:3000}];lastIdx=-1;let resolves={};sha=s=>new Promise(r=>resolves[s]=r);let old=fireBlock(0,100),fresh=fireBlock(1,100);resolves.new(new Uint8Array(32))`);
  await p.run('fresh');p.run('resolves.old(new Uint8Array(32))');await p.run('old');assert.equal(p.run('hist.length'),1);assert.equal(p.run('hist[0].height'),979777);assert.equal(p.run('organIntroUntil'),0);
});
test('live startup reconstructs remembered voicings and starts with the tip key, not a local unlock counter',async()=>{
  const p=page();audio(p);network(p);await p.run('start()');assert.equal(p.run('keyShift'),p.run('keyFor(979779)'));assert.equal(p.run('currentOrgan'),p.run('hist.at(-1).organ'));assert.equal(p.run('hist.length'),8);assert.equal(p.run('organPalette(chain[lastIdx].height).length'),9);assert.equal(p.run('organIntroUntil'),0);
  p.run('stop()');await p.run('start()');assert.equal(p.run('organPalette(chain[lastIdx].height).length'),9);
});
test('looping a future demo back to its beginning clears unlocked-chord memory and the old register',()=>{
  const p=page();audio(p);p.run(`mode='replay';playing=true;chain=[{height:979774,time:1000000,tx:3000},{height:1050000,time:2000000,tx:3000}];lastIdx=1;hist=[{chord:'Am9',notes:[],height:1050000,organ:2}];curChord='Am9';currentOrgan=2;organIntroUntil=999;setKeyFor(1050000);pad=makePad(curChord,st,100,2);rVirt0=2540000;rStart=100;tick()`);
  assert.equal(p.run('lastIdx'),-1);assert.equal(p.run('hist.length'),0);assert.equal(p.run('curChord'),'Dmaj9');assert.equal(p.run('currentOrgan'),0);assert.equal(p.run('organIntroUntil'),0);assert.equal(p.run('octShift'),p.run('octFor(979774)'));
});
test('the explicit demo link is labelled as simulation and cannot start audio without a tap',()=>{
  const p=page('?demo=1');assert.equal(p.run('mode'),'replay');assert.equal(p.run('replaySet'),'demo');assert.equal(p.run('playing'),false);assert.equal(p.run('ctx'),null);assert.equal(p.run('$("src").textContent'),'Demo · simulated blocks');assert.match(p.run('$("srcNote").textContent'),/made up/);assert.equal(p.run('$("srcDemo").hidden'),false);assert.equal(p.run('chain.length'),p.run('DEMO.length'));assert.equal(p.run('chain.every(b=>b.hash===null)'),true);
  const normal=page();assert.equal(normal.run('mode'),'live');assert.equal(normal.run('ctx'),null);
});
test('the old recorded set has no future unlocks and the simulated demo actually crosses the first addition',()=>{
  const p=page();assert.equal(p.run('TONIGHT.every(b=>organPalette(b.height).length===8)'),true);assert.equal(p.run(`DEMO.some(b=>newOrganChord(b.height)==='Gm6')`),true);assert.equal(p.run('DEMO.every(b=>b.hash===null)'),true);
});
