const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const page = fs.readFileSync(path.join(__dirname, "../../MeteoPlaneRadar/WebPage.h"), "utf8");
const script = page.match(/<script>([\s\S]*?)<\/script>/)[1];
// Keep initialization under test control while using the firmware's actual event handlers.
const source = new vm.Script(script.replace(/\bload\(\);\s*$/, ""));

function setup() {
  const elements = new Map();
  function element(id) {
    if (!elements.has(id)) {
      const listeners = {}, classes = new Set();
      elements.set(id, {
        id, checked: false, value: "", textContent: "", style: {},
        classList: {
          toggle(name, on) { if (on) classes.add(name); else classes.delete(name); },
          contains(name) { return classes.has(name); }
        },
        addEventListener(type, fn) { (listeners[type] ||= []).push(fn); },
        change() { for (const fn of listeners.change || []) fn({target:this}); },
        querySelector() { return element(id+".child"); }
      });
    }
    return elements.get(id);
  }
  const jobs = new Map(), calls = [];
  let timer = 0;
  const cfg = {screens:{bambu:true},printOn:false,bambu:{on:false,mode:"lan",region:"global",serial:"SN42",
    host:"192.168.1.2",hasCode:true,hasToken:true,pause:true,error:true,finish:true,night:0}};
  const dependencies = [];
  for (const [tag, dep] of page.matchAll(/<div\b[^>]*\bdata-dep="([^"]+)"[^>]*>/g)) {
    const block = element(tag.match(/\bid="([^"]+)"/)?.[1] || ".dep-" + dep);
    block.dataset = {dep};
    for (const name of tag.match(/\bclass="([^"]*)"/)[1].split(/\s+/)) block.classList.toggle(name, true);
    if (block.classList.contains("dep")) dependencies.push(block);
  }
  const document = {
    getElementById:element, querySelector:element, querySelectorAll:selector=>selector===".dep"?dependencies:[],
    documentElement:{},body:element("body")
  };
  const ctx = vm.createContext({document, window:{scrollTo(){}}, cfg, setInterval(){},clearTimeout(id){jobs.delete(id);},
    setTimeout(fn,ms){jobs.set(++timer,{fn,ms});return timer;},
    async fetch(url, options){
      if (options && options.body) {
        const body = JSON.parse(options.body);
        calls.push({url,body});
        if (body.bambu) for (const [k,v] of Object.entries(body.bambu)) {
          if (k==="forget") {cfg.bambu.hasCode=false;cfg.bambu.hasToken=false;}
          else if(k==="code") cfg.bambu.hasCode=true;
          else if(k==="token") cfg.bambu.hasToken=true;
          else cfg.bambu[k]=v;
        }
      }
      return {ok:true,status:200,json:async()=>options?{ok:true}:structuredClone(cfg)};
    }
  });
  source.runInContext(ctx);
  const run = code => vm.runInContext(code,ctx);
  run("CFG=JSON.parse(JSON.stringify(cfg));bambuFill();");
  async function flush(ms) { for (const [id,job] of [...jobs]) if (job.ms===ms) { jobs.delete(id);await job.fn(); } }
  return {element,run,calls,flush};
}

test("all three locales cover the form and the screen keeps stable ID 10",()=>{
  const {run,element} = setup();
  const form = page.match(/<h2>Bambu Lab<\/h2>([\s\S]*?)<\/section>/)[1];
  for (const [,key] of form.matchAll(/data-i18n="([^"]+)"/g))
    for (const lang of ["cs","en","sk"]) assert.ok(run('D.'+lang+'['+JSON.stringify(key)+']'), key);
  assert.equal(run('SCR.map(s=>s[1]).join(",")'),"0,1,2,3,4,5,7,10,8,9,6");
  assert.equal(run("TAB_VIS.tPrint()"),false);
  for(const id of ["sBambu","bambuOn","sPrint","printOn"]){
    element(id).checked=true;
    assert.equal(run("TAB_VIS.tPrint()"),true);
    element(id).checked=false;
  }
  run('drawScrBtns(10, Array.from({length:11},(_,i)=>i===10));');
  assert.match(element("scrBtns").innerHTML,/onclick='goScreen\(10\)'>Bambu Lab/);
});

test("Bambu settings follow the watch checkbox independently of the screen",()=>{
  const {element,run} = setup();
  const screen = element("sBambu"), watch = element("bambuOn"), form = element("bambuForm");
  screen.checked = true;
  run("deps();tabVis()");
  assert.ok(form.classList.contains("hide"));
  watch.checked = true;
  watch.change();
  assert.ok(!form.classList.contains("hide"));
  screen.checked = false;
  screen.change();
  assert.ok(!form.classList.contains("hide"));
  watch.checked = false;
  watch.change();
  assert.ok(form.classList.contains("hide"));
  screen.checked = true;
  screen.change();
  assert.ok(form.classList.contains("hide"));
});

test("selecting the Bambu screen stages the full screen mask",async()=>{
  const {element,flush,calls} = setup();
  element("sBambu").checked=true;element("sBambu").change();
  await flush(350);
  assert.equal(calls.length,1);
  assert.equal(calls[0].body.screens.bambu,true);
  assert.equal(calls[0].body.screens.print,false);
  assert.equal(Object.keys(calls[0].body.screens).length,10);
});

test("independent live switches never submit an edited credential",async()=>{
  const {element,flush,calls} = setup();
  element("bambuCode").value="unsaved!";
  element("bambuOn").checked=true;element("bambuOn").change();
  element("bambuPause").checked=false;element("bambuPause").change();
  await flush(350);
  assert.deepEqual(calls.map(c=>c.body),[{bambu:{on:true}},{bambu:{pause:false}}]);
  assert.match(element("haYaml").textContent,/unique_id: mpr_bambu_stav/);
  assert.doesNotMatch(element("haYaml").textContent,/unique_id: mpr_tisk_stav/);
});

test("explicit LAN and cloud saves, retaining empty values, and forgetting",async()=>{
  const {element,run,calls} = setup();
  assert.equal(element("bambuCode").value,"");
  element("bambuHost").value="999.2.3.4";await run("saveBambu()");assert.equal(calls.length,0);
  element("bambuHost").value="192.168.1.4";
  element("bambuToken").value="inactive-token";
  await run("saveBambu()");
  assert.deepEqual(calls.at(-1).body.bambu,{mode:"lan",serial:"SN42",host:"192.168.1.4"});
  element("bambuCode").value="12345678";
  await run("saveBambu()");assert.equal(calls.at(-1).body.bambu.code,"12345678");
  assert.equal(element("bambuCode").value,"");
  element("bambuMode").value="cloud";element("bambuMode").change();
  assert.equal(element("bambuLan").classList.contains("hide"),true);
  element("bambuRegion").value="cn";element("bambuToken").value="test-token";element("bambuCode").value="unused";
  await run("saveBambu()");
  assert.deepEqual(calls.at(-1).body.bambu,{mode:"cloud",serial:"SN42",region:"cn",token:"test-token"});
  assert.equal(element("bambuToken").value,"");
  await run("saveBambu(true)");assert.deepEqual(calls.at(-1).body,{bambu:{forget:true}});
});

test("printer status is plain text and Home Assistant IDs stay independent",()=>{
  const {element,run} = setup();
  run('L="en"; printStatus({state:"no_report"},"bambuSt");');
  assert.match(element("bambuSt").textContent,/waiting/i);
  run('printStatus({state:"printing",pct:75,file:"<demo>"},"bambuSt");');
  assert.match(element("bambuSt").textContent,/75 %.*<demo>/);
  assert.equal(element("bambuSt").innerHTML,undefined);
  run("CFG.printOn=true;CFG.bambu.on=true;haInfo();");
  const yaml=element("haYaml").textContent;
  assert.match(yaml,/unique_id: mpr_tryska\b/);
  assert.match(yaml,/value_json\.bambu\.nozzle_c/);
  const ids=Array.from(yaml.matchAll(/unique_id: (\w+)/g),m=>m[1]);
  assert.equal(ids.length,new Set(ids).size);
});
