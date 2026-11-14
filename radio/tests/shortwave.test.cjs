// No build/dependencies: node --test radio/tests/shortwave.test.cjs
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const {webcrypto}=require('node:crypto');
const html=fs.readFileSync(require('node:path').join(__dirname,'../index.html'),'utf8');
const scripts=[...html.matchAll(/<script[^>]*>([\s\S]*?)<\/script>/g)].map(m=>m[1]);
function page(){
  const elements=new Map(),events=[],intervals=[];
  const canvas=new Proxy({}, {get:(_,p)=>p==='setTransform'?()=>{}:()=>{}});
  const el=id=>{if(!elements.has(id))elements.set(id,{id,value:'',style:{},classList:{add(){},remove(){}},
    addEventListener(type,fn){events.push({id,type,fn})},setAttribute(){},querySelectorAll(){return []},
    getBoundingClientRect(){return {width:800}},getContext(){return canvas},closest(){return {hidden:false}}});return elements.get(id)};
  const env=vm.createContext({console,crypto:webcrypto,Uint8Array,Float32Array,TextEncoder,Date,Math,Number,Promise,
    setTimeout:()=>1,clearTimeout(){},setInterval:fn=>{intervals.push(fn);return intervals.length},clearInterval(){},AbortController,
    navigator:{},location:{hostname:'localhost'},requestAnimationFrame:fn=>fn(),
    getComputedStyle:()=>({getPropertyValue:()=>''}),
    document:{getElementById:el,querySelectorAll:()=>[],querySelector:()=>el('button')},
    window:{addEventListener(){},devicePixelRatio:1},speechSynthesis:{cancel(){}}});
  vm.runInContext(scripts.at(-1),env);
  return {env,run:code=>vm.runInContext(code,env),elements,events,intervals};
}
function audio(p){
  p.env.nodes=[];
  p.run(`function param(value=0){return {value,events:[],setValueAtTime(v,t){this.value=v;this.events.push(['set',v,t])},linearRampToValueAtTime(v,t){this.events.push(['ramp',v,t])},exponentialRampToValueAtTime(v,t){this.events.push(['exp',v,t])},setTargetAtTime(v,t,c){this.events.push(['target',v,t,c])},cancelScheduledValues(t){this.events.push(['cancel',t])}}}
    function node(kind){const n={kind,gain:param(),frequency:param(),detune:param(),Q:param(),connect(){},start(t){this.started=t},stop(t){this.stopped=t}};nodes.push(n);return n}
    ctx={currentTime:0,createGain:()=>node('gain'),createOscillator:()=>node('osc'),createBiquadFilter:()=>node('filter'),createWaveShaper:()=>node('shaper'),createStereoPanner:()=>{const n=node('pan');n.pan=param();return n}};
    bus=node('bus');out=node('out');`);
}
const H='a'.repeat(64),P='b'.repeat(64),TIP='c'.repeat(64),OTHER='d'.repeat(64),B=689472;
function retuneFixture(p,{change=10,active=H}={}){
  p.env.fixture={B,H,P,TIP,OTHER,active,b:{height:B,id:H,previousblockhash:P,difficulty:110},prev:{height:B-1,id:P,difficulty:100}};
  p.env.requestedChange=change;
  p.run(`fixture.prev.bits=0x17010000;
    fixture.b.bits=packTarget(BigInt(Math.round(Number(mainnetTarget(fixture.prev.bits))/(1+requestedChange/100))));
    fixture.prev.difficulty=Number(DIFF_ONE_TARGET)/Number(mainnetTarget(fixture.prev.bits));
    fixture.b.difficulty=Number(DIFF_ONE_TARGET)/Number(mainnetTarget(fixture.b.bits));
    playing=true;mode='live';sourceRun=1;chain=[{height:fixture.B+5,hash:fixture.TIP}];lastIdx=0;
    getJSON=async url=>{if(url.includes('/block-height/'))return url.endsWith('/'+fixture.B)?fixture.active:fixture.TIP;if(url.endsWith(fixture.H))return fixture.b;if(url.endsWith(fixture.P))return fixture.prev;throw Error(url)};`);
}
test('all inline scripts parse; no audio before a tap',()=>{
  scripts.forEach(s=>new vm.Script(s));const p=page();assert.equal(p.run('ctx'),null);assert.equal(p.run('playing'),false);
});
test('hash palette is deterministic; direction/size are bounded and unknown is neutral',()=>{
  const p=page();assert.equal(p.run('shortwavePlan(null,null).brightness'),1);
  assert.equal(p.run('shortwavePlan(null,0).activity'),1);
  assert.ok(p.run('shortwavePlan(null,5).brightness>shortwavePlan(null,0).brightness'));
  assert.ok(p.run('shortwavePlan(null,-5).activity<shortwavePlan(null,0).activity'));
  assert.equal(p.run('shortwavePlan(null,500).activity'),1.25);
  assert.equal(p.run('shortwavePlan(null,-500).activity'),0.75);
  assert.ok(p.run('shortwavePlan(null,Infinity).brightness===1'));
  assert.equal(p.run('new Set([0,1,2,3].map(n=>{const b=new Uint8Array(32);b[12]=n;return shortwavePlan(b,0).voicing})).size'),4);
  assert.equal(p.run('JSON.stringify(shortwavePlan(new Uint8Array(32).fill(19),2))===JSON.stringify(shortwavePlan(new Uint8Array(32).fill(19),2))'),true);
});
test('epoch key, halving register, call sign and eight-block memory are preserved',async()=>{
  const p=page();assert.equal(p.run('((keyFor(2016)-keyFor(2015))%12+12)%12'),7);
  assert.equal(p.run('octFor(1050000)-octFor(1049999)'),-12);
  const sign=await p.run(`songBytes({hash:'0000000000000000000320283a032748cef8227873ff4872689bf23f1cda83a5',height:840000}).then(callSign)`);
  assert.equal(sign,'BW0WY');p.run('for(let i=0;i<12;i++)remember("Dmaj9",[],i,new Uint8Array(32))');
  assert.equal(p.run('hist.length'),8);assert.equal(p.run('hist[0].height'),4);
});
test('organ remains three square voices; motif follows the key and halving shift',()=>{
  const p=page();audio(p);p.run('setKeyFor(975744);makePad("Dmaj9","shortwave",0)');
  assert.equal(p.env.nodes.filter(n=>n.kind==='osc').length,3);
  assert.ok(p.env.nodes.filter(n=>n.kind==='osc').every(n=>n.type==='square'));
  p.env.nodes.length=0;p.run('intervalSignal([{m:69,d:0.32}],1,1,-12,0.18)');
  const frequencies=p.env.nodes.filter(n=>n.kind==='osc').map(n=>n.frequency.value);
  assert.ok(frequencies.includes(220));assert.ok(!frequencies.includes(440));
});
test('chapters include a genuine organ rest and silence the interval signals',()=>{
  const p=page();audio(p);p.run(`hist=[{height:975744,chord:'Dmaj9',notes:[{m:69,d:0.32}],by:new Uint8Array(32)}];
    chain=[{height:975744}];lastIdx=0;harm.next=0;waitMin=()=>4;let levels=[];pad={level:x=>levels.push(x)};shortwaveTick(100,0.5)`);
  assert.equal(p.run('levels.at(-1)'),0);assert.equal(p.run('swSignal'),null);
});
test('speech/Morse preempts a motif; Stop cancels the new phrase',()=>{
  const p=page();audio(p);p.run(`let cancelled=0;swSignal={until:100,stop(){cancelled++}};
    morse=()=>1;readNumber=(h,t)=>t+1;say=(tokens,t)=>t+1;transmit(975745,new Uint8Array(32),1);`);
  assert.equal(p.run('cancelled'),1);assert.equal(p.run('swSignal'),null);
  p.run('swSignal={stop(){cancelled++}};stop()');assert.equal(p.run('cancelled'),2);
});
test('the busy window prevents melodies, without suppressing future quiet phrases',()=>{
  const p=page();audio(p);p.run(`hist=[{height:975744,chord:'Dmaj9',notes:[{m:69,d:0.32}],by:new Uint8Array(32)}];
    chain=[{height:975744}];lastIdx=0;waitMin=()=>0.5;pad=null;swBusy=100;
    let phrases=0;intervalSignal=()=>{phrases++;return {until:0,stop(){}}};shortwaveTick(20,0.1)`);
  assert.equal(p.run('phrases'),0);p.run('swBusy=0;harm.next=0;harm.count=0;shortwaveTick(110,0.1)');assert.equal(p.run('phrases'),1);
});
test('retarget requires the exact boundary, hashes, link, numeric positive difficulty',()=>{
  const p=page();retuneFixture(p);
  assert.ok(Math.abs(p.run('retargetPair(fixture.b,fixture.prev,fixture.B,fixture.H).change')-10)<1e-3);
  for(const mutation of ["b.height++","prev.height--","b.id=fixture.OTHER","prev.id=fixture.OTHER","b.previousblockhash='bad'","b.difficulty='110'","b.difficulty=null","b.difficulty=0","prev.difficulty=-1","b.difficulty=Infinity"]){
    retuneFixture(p);p.run(`fixture.${mutation}`);
    assert.equal(p.run('retargetPair(fixture.b,fixture.prev,fixture.B,fixture.H)'),null,mutation);
  }
  retuneFixture(p);assert.equal(p.run('retargetPair(fixture.b,fixture.prev,fixture.B+1,fixture.H)'),null);
});
test('active boundary lookup authorizes only the same accepted tip/epoch; 0% is valid',async()=>{
  const p=page();retuneFixture(p,{change:0});await p.run('fetchRetune(chain[0])');
  assert.equal(p.run('retuneChange(fixture.B+5)'),0);
  assert.equal(p.run('retuneChange(fixture.B+2016)'),null);
  p.run('chain[0].hash=fixture.OTHER');assert.equal(p.run('retuneChange(fixture.B+5)'),null);
});
test('orphan, failed lookup, superseded request and source restart all fall back to neutral',async()=>{
  for(const action of ['orphan','failure','source','request','mode']){
    const p=page();retuneFixture(p);
    p.env.action=action;
    p.run(`let lookups=0;const original=getJSON;getJSON=async url=>{
      if(url.includes('/block-height/')&&++lookups===2){
        if(action==='orphan')return fixture.OTHER;if(action==='failure')throw Error('offline');
        if(action==='source')sourceRun++;if(action==='request')retuneRequest++;if(action==='mode')mode='replay';
      }return original(url)};`);
    await p.run('fetchRetune(chain[0])');assert.equal(p.run('retune'),null,action);
  }
});
test('a stale/fork tip cannot authorize an otherwise active boundary',async()=>{
  const p=page();retuneFixture(p);
  p.run(`const original=getJSON;getJSON=async url=>url.endsWith('/'+(fixture.B+5))?fixture.OTHER:original(url)`);
  await p.run('fetchRetune(chain[0])');assert.equal(p.run('retune'),null);
});
test('hash work finishing after Stop or a new accepted snapshot cannot mutate history',async()=>{
  for(const counter of ['sourceRun','chainVersion']){
    const p=page();audio(p);
    p.run(`playing=true;chain=[{height:975744,time:0,hash:null}];lastIdx=-1;
      let finishHash;sha=()=>new Promise(resolve=>finishHash=resolve);let work=fireBlock(0,1);${counter}++;
      hist=[];finishHash(new Uint8Array(32));`);
    await p.run('work');assert.equal(p.run('hist.length'),0,counter);
  }
});
test('percentage announcement requires the exact boundary block hash, not just its height',async()=>{
  const p=page();retuneFixture(p);await p.run('fetchRetune(chain[0])');
  assert.notEqual(p.run('adjResult({height:fixture.B,hash:fixture.H})'),null);
  assert.equal(p.run('adjResult({height:fixture.B,hash:fixture.OTHER})'),null);
  assert.equal(p.run('adjResult({height:fixture.B+1,hash:fixture.H})'),null);
});
test('mutable API lookups bypass HTTP cache; immutable block records may be cached',async()=>{
  const p=page();const calls=[];
  p.env.fetch=async(url,options)=>{calls.push({url,options});return {ok:true,headers:{get:()=>''},text:async()=>''}};
  await p.run(`getJSON(API+'/block-height/689472')`);
  await p.run(`getJSON(API+'/blocks/tip/hash')`);
  await p.run(`getJSON(API+'/block/'+'a'.repeat(64))`);
  assert.deepEqual(calls.map(c=>c.options.cache),['no-store','no-store','default']);
});
test('inconsistent observed-tip/list snapshots cannot authorize new playback',async()=>{
  const p=page();retuneFixture(p);
  p.run(`const original=getJSON;getJSON=async url=>url.endsWith('/blocks/tip/hash')?fixture.OTHER:
    url.endsWith('/v1/blocks')?[{id:fixture.H,height:fixture.B+6}]:url.endsWith('/mempool')?{count:100}:original(url);livePoll()`);
  await p.intervals.at(-1)();assert.equal(p.run('retune'),null);assert.equal(p.run('chain.length'),1);
});
test('demo has only its labelled fake adjustment; recording/estimate never colour live music',()=>{
  const p=page();p.run(`mode='replay';replaySet='tonight'`);assert.equal(p.run('retuneChange(975744)'),null);
  p.run(`replaySet='demo'`);assert.equal(p.run('retuneChange(975744)'),2.1);assert.equal(p.run('retuneChange(1050000)'),null);
  p.run(`mode='live';adj={change:99,prev:99}`);assert.equal(p.run('adjResult(975744)'),null);
});
