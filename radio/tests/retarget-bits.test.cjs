// Bitcoin's retarget vectors, exercised against the actual inline browser helper.
'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs'),vm=require('node:vm'),path=require('node:path');
const html=fs.readFileSync(path.join(__dirname,'../index.html'),'utf8');
const source=html.match(/\/\/ Mainnet-only numeric consistency helper[\s\S]*?\/\/ End retarget numeric helper\./)[0];
const {checkedMainnetRetargetPct:checked,mainnetTarget:target,packTarget:pack,unpackTarget:unpack,
  MAINNET_POW_LIMIT:limit,DIFF_ONE_TARGET:one}=vm.runInNewContext(source+`\n;({checkedMainnetRetargetPct,
    mainnetTarget,packTarget,unpackTarget,MAINNET_POW_LIMIT,DIFF_ONE_TARGET})`);
const difficulty=bits=>Number(one)/Number(target(bits));
const check=(oldBits,newBits)=>checked(oldBits,newBits,difficulty(oldBits),difficulty(newBits));
const approx=(actual,expected)=>assert.ok(actual!==null&&Math.abs(actual-expected)<1e-10,`${actual} != ${expected}`);
// Bitcoin Core v28.0 src/test/pow_tests.cpp hardcoded mainnet vectors.
for(const [oldBits,newBits] of [[0x1d00ffff,0x1d00d86a],[0x1d00ffff,0x1d00ffff],
  [0x1c05a3f4,0x1c0168fd],[0x1c387f6f,0x1d00e1fd]])assert.notEqual(check(oldBits,newBits),null);
assert.equal(check(0x1c05a3f4,0x1c0168fc),null);
assert.equal(check(0x1c387f6f,0x1d00e1fe),null);
console.log('PASS: four Core retarget vectors + two boundary rejection vectors');
// Mempool /api/v1/blocks/689472 snapshot, fetched by Bitcoin 2026-11-14.
approx(checked(0x170e1ef9,0x171398ce,19932791027262.74,14363025673659.96),-27.942726866422397);
approx(check(0x17020003,0x17008000),300.0091552734375);
assert.equal(check(0x17020003,0x167fffff),null);
approx(check(0x17010000,0x17040000),-75);
approx(check(0x17010000,0x17010000),0);
approx(check(0x1d00ffff,0x1d00ffff),0);
// Hypothetical tiny targets: this checks bounds, not a historical mainnet event.
approx(check(0x01070000,0x01010000),600);
console.log('PASS: historical -27.9427%, +300.009155%, -75%, zero, tiny-target edge');
for(const invalid of [null,undefined,true,false,'386801401',NaN,Infinity,-Infinity,0,-1,1.5,
  0x100000000,0x1d80ffff,0x23000001,0x1d010000,0x01010001,0x01000001]){
  assert.equal(target(invalid),null,`accepted malformed bits ${invalid}`);
  assert.equal(checked(invalid,0x171398ce,1,1),null);
}
for(const invalid of [null,undefined,true,'19932791027262.74',NaN,Infinity,-Infinity,0,-1,
  Number.MIN_VALUE,Number.MAX_VALUE,difficulty(0x170e1ef9)*1.001]){
  assert.equal(checked(0x170e1ef9,0x171398ce,invalid,difficulty(0x171398ce)),null);
}
assert.equal(checked(0x170e1ef9,0x171398ce,difficulty(0x170e1ef9),difficulty(0x171398ce)*1.001),null);
console.log('PASS: malformed/noncanonical bits and inconsistent/nonfinite difficulty');
// Deterministic coverage over canonical targets and compact byte/sign boundaries.
let seed=0x12345678,generated=0;
const rand=()=>(seed=(Math.imul(seed,1664525)+1013904223)>>>0);
for(let i=0;i<10000;i++){
  const raw=BigInt(1+rand()%0x7fffff)<<BigInt(8*(rand()%26));
  const oldBits=pack(raw>limit?limit:raw),oldTarget=target(oldBits);
  assert.notEqual(oldTarget,null);assert.equal(pack(oldTarget),oldBits);
  const span=BigInt(302400+rand()%(4838400-302400+1));
  let next=oldTarget*span/1209600n;if(next>limit)next=limit;
  const newBits=pack(next);if(!newBits)continue;
  assert.notEqual(check(oldBits,newBits),null);generated++;
}
console.log(`PASS: ${generated} deterministic generated retargets (Node harness, not native Core)`);
