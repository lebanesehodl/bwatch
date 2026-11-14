// No build/dependencies: node --test radio/tests/playback-lifecycle.test.cjs
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),vm=require('node:vm'),path=require('node:path');
const {webcrypto}=require('node:crypto');
const html=fs.readFileSync(process.env.RADIO_SOURCE||path.join(__dirname,'../index.html'),'utf8');
const source=[...html.matchAll(/<script[^>]*>([\s\S]*?)<\/script>/g)].at(-1)[1];
function page(){
  const clock={wall:1_800_000_000_000,mono:0},elements=new Map(),intervals=[],timeouts=[];
  class ClockDate extends Date{static now(){return clock.wall}}
  const canvas=new Proxy({}, {get:()=>()=>{}});
  const el=id=>{if(!elements.has(id))elements.set(id,{id,value:'',style:{},classList:{add(){},remove(){}},addEventListener(){},setAttribute(){},querySelectorAll(){return []},getBoundingClientRect(){return {width:800}},getContext(){return canvas},closest(){return {hidden:false}}});return elements.get(id)};
  const env=vm.createContext({console,crypto:webcrypto,TextEncoder,Uint8Array,Float32Array,Date:ClockDate,performance:{now:()=>clock.mono},Math,Number,Promise,AbortController,
    setTimeout:fn=>{timeouts.push(fn);return timeouts.length},clearTimeout(){},setInterval:fn=>{intervals.push(fn);return intervals.length},clearInterval(){},
    navigator:{},location:{hostname:'localhost'},requestAnimationFrame:fn=>fn(),getComputedStyle:()=>({getPropertyValue:()=>''}),
    document:{getElementById:el,querySelectorAll:()=>[],querySelector:()=>el('button')},window:{addEventListener(){},devicePixelRatio:1},speechSynthesis:{cancel(){}}});
  vm.runInContext(source,env);
  return {env,clock,elements,intervals,timeouts,run:code=>vm.runInContext(code,env)};
}
function audio(p){p.run(`
  ctx={currentTime:100,state:'running'};let pads=[];
  makePad=()=>{const p={stopped:false,tension(){},stop(){this.stopped=true}};pads.push(p);return p};
  initAudio=unlockAudio=startBed=stopBed=trim=loadFor=draw=presenceOn=presenceOff=()=>{};
  playBlock=grain=()=>{};
`)}
const apiBlock=(n,t)=>({id:n.toString(16).padStart(64,'0'),height:970000+n,timestamp:t/1000,tx_count:3000});
function network(p){
  p.env.fixture=Array.from({length:10},(_,i)=>apiBlock(10-i,p.clock.wall-(i+1)*600000));
  p.run(`getJSON=async url=>url.endsWith('/v1/blocks')?fixture:url.endsWith('/mempool')?{count:3000}:{};`);
}
test('a future-dated live block cannot pin the wait clock and chord window at zero',async()=>{
  const p=page();audio(p);network(p);p.env.fixture[0].timestamp=(p.clock.wall+30*60000)/1000;
  await p.run('start()');assert.equal(p.run('waitMin()'),0);
  assert.equal(p.run('chain[lastIdx].time'),p.env.fixture[0].timestamp*1000,'the source timestamp itself must not be rewritten');
  p.clock.wall+=60000;p.clock.mono+=60000;
  assert.equal(p.run('waitMin()'),1,'elapsed observed time must advance despite the miner timestamp');
  p.run(`let heard=[];setChord=ch=>heard.push(ch);for(let i=0;i<45;i++)harmonyTick(101+i,tensionOf(waitMin()))`);
  assert.ok(p.run('new Set(heard).size')>1,'history must not remain pinned to one chord');
});
test('late/background-throttled ticks advance once instead of postponing forever',async()=>{
  const p=page();audio(p);network(p);await p.run('start()');
  p.run(`let heard=[];setChord=ch=>heard.push(ch);for(let i=1;i<=5;i++)harmonyTick(100+i*60,0.8)`);
  assert.equal(p.run('heard.length'),5,'one overdue change per tick, not an endlessly deferred deadline');
  assert.ok(p.run('new Set(heard).size')>1);
});
test('normal live age is preserved; moving the wall clock backward cannot freeze it',async()=>{
  const p=page();audio(p);network(p);await p.run('start()');assert.equal(p.run('waitMin()'),10);
  p.clock.wall-=3600000;p.clock.mono+=60000;
  assert.equal(p.run('waitMin()'),11);
});
test('Stop/restart ignores an old startup and cannot strand a second held pad',async()=>{
  const p=page();audio(p);network(p);
  p.run(`let resolveOld;const realGet=getJSON;let gets=0;getJSON=url=>url.endsWith('/v1/blocks')&&gets++===0?new Promise(r=>resolveOld=r):realGet(url);let old=start()`);
  p.run('stop()');await p.run('start()');assert.equal(p.run('pads.filter(p=>!p.stopped).length'),1);
  p.run('resolveOld(fixture)');await p.run('old');
  assert.equal(p.run('pads.filter(p=>!p.stopped).length'),1,'stale startup must not create an orphan pad');
  p.run('stop()');assert.equal(p.run('pads.filter(p=>!p.stopped).length'),0,'Stop must leave no held pad behind');
});
test('a pending poll cannot mutate a restarted player',async()=>{
  const p=page();audio(p);network(p);await p.run('start()');
  p.run(`let resolvePoll;const realGet=getJSON;getJSON=url=>url.endsWith('/blocks/tip/hash')?new Promise(r=>resolvePoll=r):realGet(url);livePoll()`);
  const callback=p.intervals.at(-1),work=callback();p.run('stop()');await p.run('start()');
  const expected=p.run('JSON.stringify(chain)');p.env.fixture=[apiBlock(11,p.clock.wall),...p.env.fixture];
  p.run(`resolvePoll('f'.repeat(64))`);await work;
  assert.equal(p.run('JSON.stringify(chain)'),expected);assert.equal(p.run('pads.filter(p=>!p.stopped).length'),1);
});
test('a failed old startup cannot force a new replay into the Live-failed fallback',async()=>{
  const p=page();audio(p);network(p);
  p.run(`let rejectOld;const realGet=getJSON;let gets=0;getJSON=url=>url.endsWith('/v1/blocks')&&gets++===0?new Promise((r,j)=>rejectOld=j):realGet(url);let old=start()`);
  p.run(`stop();mode='replay'`);await p.run('start()');p.run(`rejectOld(Error('offline'))`);await p.run('old');
  assert.equal(p.run('mode'),'replay');assert.notEqual(p.elements.get('srcLive').disabled,true);
  assert.equal(p.run('pads.filter(p=>!p.stopped).length'),1);
});
test('an old block digest cannot resume audio or add history after Stop',async()=>{
  const p=page();audio(p);network(p);await p.run('start()');
  p.run(`let finish;sha=()=>new Promise(r=>finish=r);let work=fireBlock(lastIdx,ctx.currentTime);stop();hist=[];finish(new Uint8Array(32))`);
  await p.run('work');assert.equal(p.run('hist.length'),0);assert.equal(p.run('pads.filter(p=>!p.stopped).length'),0);
});
test('queued tune-in speech belongs to its playback run',async()=>{
  const p=page();p.run('const productLoadFor=loadFor');audio(p);network(p);await p.run('start()');
  p.run('loadFor=productLoadFor;loadShots=()=>{};let transmissions=0;transmit=()=>{transmissions++;return 0};loadFor(st)');
  const old=p.timeouts.at(-1);p.run('stop()');await p.run('start()');await old();
  assert.equal(p.run('transmissions'),0);
});
test('slow live polls cannot overlap',async()=>{
  const p=page();audio(p);network(p);await p.run('start()');
  p.run(`let resolves=[],pollReads=0;const realGet=getJSON;getJSON=url=>url.endsWith('/blocks/tip/hash')?(pollReads++,new Promise(r=>resolves.push(r))):realGet(url);livePoll()`);
  const callback=p.intervals.at(-1),work=callback(),overlap=callback(),reads=p.run('pollReads');
  p.run('resolves.forEach(resolve=>resolve(chain[chain.length-1].hash))');await Promise.all([work,overlap]);assert.equal(reads,1);
});
test('an interrupted audio context is resumed from the next Play tap',async()=>{
  const p=page();audio(p);network(p);
  p.run(`ctx.state='interrupted';let resumes=0;ctx.resume=async()=>{resumes++;ctx.state='running'}`);await p.run('start()');
  assert.equal(p.run('resumes'),1);assert.equal(p.run('playing'),true);
});
test('recorded replay keeps its own speed-scaled timeline',async()=>{
  const p=page();audio(p);p.run(`mode='replay';speed=2;chain=[{height:1,time:1000000}];lastIdx=0;rVirt0=1000000;rStart=100;ctx.currentTime=102`);
  assert.equal(p.run('waitMin()'),1);
});
