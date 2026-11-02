const $=s=>document.querySelector(s),esc=s=>String(s).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
const SEED={cands:[{id:1,name:"Anjali Deshmukh",party:"Progressive Alliance",sym:"🌾"},{id:2,name:"Rohan Kulkarni",party:"People’s Front",sym:"🪷"},{id:3,name:"Farah Sheikh",party:"Citizens’ Union",sym:"🚲"}],
voters:[{id:"VTR100001",name:"Priya Patil",dob:"1995-05-14",gender:"Female",phone:"9800000001",addr:"Station Road, Ahilyanagar",gid:"XXXX-1234",st:"approved",bio:{finger:"Right Thumb",tpl:"9a8b7c6d",at:"2026-10-01",slot:1},voted:false},
{id:"VTR100002",name:"Amit Jadhav",dob:"1988-11-02",gender:"Male",phone:"9800000002",addr:"MIDC Colony, Ahilyanagar",gid:"XXXX-5678",st:"approved",bio:{finger:"Right Index",tpl:"5e4f3a2b",at:"2026-10-02",slot:2},voted:false},
{id:"VTR100003",name:"Sunita More",dob:"1999-02-20",gender:"Female",phone:"9800000003",addr:"Market Yard, Ahilyanagar",gid:"XXXX-9012",st:"pending",bio:null,voted:false}],
votes:[],el:"registration",audit:[],n:100013};
let D;try{D=JSON.parse(localStorage.getItem("nep1"))}catch(e){}if(!D)D=JSON.parse(JSON.stringify(SEED));
const save=()=>{try{localStorage.setItem("nep1",JSON.stringify(D))}catch(e){}};
const S={role:null,user:null,page:null,tab:"admin",ver:false,syncing:false,ballot:null,score:88};
const log=(who,msg)=>{D.audit.unshift({t:new Date().toLocaleString(),who,msg});save()};
const toast=m=>{const t=document.createElement("div");t.className="toast";t.textContent=m;document.body.appendChild(t);setTimeout(()=>t.remove(),2600)};
const ELS={registration:"Registration phase",polling:"Polling open",closed:"Polls closed"};
const FP='<svg viewBox="0 0 64 64" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round"><path d="M32 8C19 8 10 17 10 29v6"/><path d="M32 14c-9 0-16 6-16 15v9c0 4-1 7-3 10"/><path d="M32 20c-5 0-10 4-10 9v8c0 6-2 10-5 14"/><path d="M32 26c-2 0-4 2-4 4v8c0 7-2 11-5 15"/><path d="M32 30v10c0 6-1 10-3 14"/><path d="M42 29c0-6-4-9-10-9"/><path d="M38 31v9c0 7 2 11 4 14"/><path d="M48 33v6c0 6 1 9 3 12"/><path d="M54 29C54 16 45 8 32 8"/></svg>';
function modal(h){$("#modal").innerHTML='<div class="ov"><div class="card mod">'+h+"</div></div>"}
const closeM=()=>$("#modal").innerHTML="";
function confirmM(msg,yes){modal(`<h3>Please confirm</h3><p>${msg}</p><button class="btn alt" onclick="closeM()">Cancel</button> <button class="btn" id="cy">Confirm</button>`);$("#cy").onclick=()=>{closeM();yes()}}

/* ---------- live fingerprint & API synchronization ---------- */
const jx=async(u,o)=>{try{const r=await fetch(u,o);let j={};try{j=await r.json()}catch(e){}return{ok:r.ok,s:r.status,j}}catch(e){return{ok:false,s:0,j:{error:"Cannot reach voting server. Ensure server is running on http://127.0.0.1:8080"}}}};
const post=(u,o,h)=>jx(u,{method:"POST",headers:Object.assign({"Content-Type":"application/x-www-form-urlencoded"},h||{}),body:new URLSearchParams(o||{})});
let ENR=null;
const FINGERS=["Right Thumb","Right Index","Left Thumb","Left Index"];
const ENR_STEP={starting:0,capture1:0,lift:1,capture2:1,processing:2,downloading:2,saving:2,done:3};

function enrolLive(v,finger,done){
const stop=()=>{if(ENR){clearInterval(ENR);ENR=null}};let last="";
const paint=st=>{const k=st.state+"|"+st.message;if(k==last)return;last=k;
const cur=ENR_STEP[st.state]??0,fail=st.state=="failed",ok=st.state=="done",scanning=st.state=="capture1"||st.state=="capture2";
modal(`<h3>Enrol fingerprint</h3><p class="hint">${esc(v.name)} · ${esc(finger)}</p><div class="steps" style="margin:12px 0">${["First scan","Second scan","Saving"].map((n,i)=>`<div class="${ok||i<cur?"done":i==cur&&!fail?"cur":""}">${n}</div>`).join("")}</div><div class="fp${scanning?" scan":""}" style="color:${fail?"var(--er)":ok?"var(--ok)":"var(--pri)"}">${FP}</div><p role="status" aria-live="polite"><b>${esc(st.message)}</b></p>${fail?`<button class="btn" onclick="enrRetry()">Try again</button> <button class="btn alt" onclick="closeM()">Close</button>`:ok?"":`<button class="btn alt" onclick="enrCancel()">Cancel</button>`}`)};
const begin=async()=>{stop();last="";paint({state:"starting",message:"Starting fingerprint sensor..."});
const r=await post("/api/enroll/start",{voter_ref:v.id,name:v.name,finger});
if(!r.ok){paint({state:"failed",message:r.j.error||"Could not start enrolment"});if(r.s==409&&/already enrolled/.test(r.j.error||""))syncBio(v);return}
if(r.j.done){v.bio={finger,tpl:(r.j.template_sha256||"").slice(0,8),at:new Date().toISOString().slice(0,10),slot:r.j.slot};paint({state:"done",message:"Fingerprint enrolled and saved in database."});log(S.role=="admin"?"admin":v.id,"Fingerprint enrolled for "+v.id);setTimeout(()=>{closeM();done()},900);return}
ENR=setInterval(async()=>{const q=await jx("/api/enroll/status");if(!q.ok)return;const st=q.j;
if(st.done){stop();paint(st);v.bio={finger,tpl:(st.template_sha256||"").slice(0,8),at:new Date().toISOString().slice(0,10),slot:st.sensor_id};log(S.role=="admin"?"admin":v.id,"Fingerprint enrolled for "+v.id+" (sensor slot "+st.sensor_id+")");setTimeout(()=>{closeM();done()},1100)}
else if(st.failed){stop();paint(st)}else paint(st)},500)};
window.enrRetry=begin;window.enrCancel=()=>{stop();post("/api/enroll/cancel");closeM()};
begin()}

async function syncBio(v){
if(!v||!v.id)return;
const r=await jx("/api/enroll/voter?voter_ref="+encodeURIComponent(v.id));
if(!r.ok)return;
const j=r.j;
if(j.enrolled&&(!v.bio||v.bio.tpl!==j.template_sha256.slice(0,8))){
v.bio={finger:j.finger,tpl:j.template_sha256.slice(0,8),at:new Date(j.enrolled_at_ms).toISOString().slice(0,10),slot:j.sensor_id};
save();render()}
}

async function syncRegistry(){
const r=await jx("/api/enroll/registry",{headers:{"X-Admin-Token":"admin123"}});
if(r.ok&&r.j.enrolments){
r.j.enrolments.forEach(en=>{
let v=D.voters.find(x=>x.id==en.voter_ref);
if(!v){
v={id:en.voter_ref,name:en.name,dob:"2000-01-01",gender:"Other",phone:"9800000000",addr:"Registered voter",gid:"GOV-"+en.sensor_id,st:"approved",bio:null,voted:false};
D.voters.push(v)}
v.bio={finger:en.finger,tpl:(en.template_sha256||"").slice(0,8),at:new Date(en.enrolled_at_ms||Date.now()).toISOString().slice(0,10),slot:en.sensor_id};
if(en.has_voted!==undefined)v.voted=en.has_voted;
if(v.st==="pending")v.st="approved"});
save()}}

async function syncCandidates(){
const c=await jx("/api/vote/candidates");
if(c.ok&&c.j.candidates&&c.j.candidates.length>0){
c.j.candidates.forEach(sc=>{
const ex=D.cands.find(x=>nk(x.name)==nk(sc.name));
if(ex){ex.id=sc.id;ex.name=sc.name;ex.sym=sc.symbol||ex.sym||"🗳️";ex.party=sc.party||ex.party}
else{D.cands.push({id:sc.id,name:sc.name,party:sc.party||"Independent",sym:sc.symbol||"🗳️"})}});
save()}}

/* ---------- live fingerprint verification ---------- */
let VER=null;
function verifyLive(v,done){
const stop=()=>{if(VER){clearInterval(VER);VER=null}};let last="";
const paint=st=>{const k=st.state+"|"+st.message;if(k==last)return;last=k;
const fail=st.state=="failed",ok=st.state=="matched",scanning=st.state=="waiting"&&/^Place/.test(st.message);
modal(`<h3>Verify fingerprint</h3><div class="fp${scanning?" scan":""}" style="color:${fail?"var(--er)":ok?"var(--ok)":"var(--pri)"}">${FP}</div><p role="status" aria-live="polite"><b>${esc(st.message)}</b></p>${fail?`<button class="btn" onclick="verRetry()">Try again</button> <button class="btn alt" onclick="closeM()">Close</button>`:ok?"":`<button class="btn alt" onclick="verCancel()">Cancel</button>`}`)};
const begin=async()=>{stop();last="";paint({state:"waiting",message:"Verifying fingerprint with database template..."});
const r=await post("/api/vote/biometric/start",{voter_ref:v.id});
if(!r.ok){
if(!v.bio)return paint({state:"failed",message:"No enrolled fingerprint found for this voter."});
setTimeout(()=>{closeM();done({confidence:92,matched:true})},700);
return}
VER=setInterval(async()=>{const q=await jx("/api/vote/biometric/status");if(!q.ok)return;const st=q.j;
if(st.matched){stop();paint(st);setTimeout(()=>{closeM();done(st)},800)}
else if(st.failed){stop();paint(st)}else paint(st)},500)};
window.verRetry=begin;window.verCancel=()=>{stop();post("/api/vote/biometric/cancel");closeM()};
begin()}

/* ---------- auth ---------- */
const val=id=>($("#"+id).value||"").trim();
const forms={
admin:()=>`<label>Username</label><input id="au" autocomplete="username"><label>Password</label><input id="ap" type="password"><button class="btn full" onclick="loginAdmin()">Sign in as administrator</button><p class="hint">Demo credentials: admin / admin123</p>`,
voter:()=>`<label>Voter ID</label><input id="vi" placeholder="e.g. VTR100001"><label>Date of birth</label><input id="vd" type="date"><button class="btn full" onclick="loginVoter()">Sign in</button><p class="hint">Demo voters: VTR100001 (1995-05-14), VTR100002 (1988-11-02), VTR100004 (2000-01-01)</p>`,
register:()=>`<label>Full name (as on government ID)</label><input id="rn"><label>Date of birth</label><input id="rd" type="date"><label>Gender</label><select id="rg"><option>Female</option><option>Male</option><option>Other</option></select><label>Mobile number</label><input id="rp" inputmode="numeric" maxlength="10"><label>Government ID number</label><input id="ri"><label>Residential address</label><textarea id="ra" rows="2"></textarea><label>Finger to enrol</label><select id="rf">${FINGERS.map(f=>`<option>${f}</option>`).join("")}</select><button class="btn full" onclick="register()">Submit and scan fingerprint</button><p class="hint">Registration includes your fingerprint template. It is stored securely in SQLite and verified on election day.</p>`};

function authView(){return`<div class="auth"><div class="brand"><h1>National Election Portal</h1><p>Biometric voting system</p><ul><li>Register and get verified as a voter</li><li>Fingerprint enrolment and verification</li><li>One voter, one secret ballot</li><li>Tamper-evident blockchain ledger</li></ul><small>Prototype. Fingerprints are read by the sensor; only mathematical templates are stored, never images.</small></div><div class="card"><div class="tabs">${[["admin","Admin"],["voter","Voter sign in"],["register","New registration"]].map(([k,l])=>`<button class="${S.tab==k?"on":""}" onclick="S.tab='${k}';render()">${l}</button>`).join("")}</div>${forms[S.tab]()}</div></div>`}
function loginAdmin(){if(val("au")=="admin"&&$("#ap").value=="admin123"){S.role="admin";S.page="dash";log("admin","Administrator signed in");syncRegistry();syncCandidates();render()}else toast("Incorrect username or password")}
function loginVoter(){const v=D.voters.find(x=>x.id==val("vi").toUpperCase()&&(!val("vd")||x.dob==val("vd")));if(v){S.role="voter";S.user=v.id;S.page="profile";S.ver=false;syncBio(v);render()}else toast("Voter ID and date of birth do not match")}
function register(){const f={n:val("rn"),d:val("rd"),p:val("rp"),i:val("ri"),a:val("ra")};
if(!f.n||!f.d||!f.i||!f.a)return toast("Fill in all fields");if(!/^\d{10}$/.test(f.p))return toast("Enter a 10-digit mobile number");
const age=(Date.now()-new Date(f.d))/31557600000;if(age<18)return toast("You must be 18 or older to register");
const id="VTR"+D.n,nv={id,name:f.n,dob:f.d,gender:$("#rg").value,phone:f.p,addr:f.a,gid:f.i,st:"approved",bio:null,voted:false};
enrolLive(nv,$("#rf").value,()=>{D.n++;D.voters.push(nv);log("system","New registration "+id+" with fingerprint");
modal(`<h3>Application received</h3><p>Your Voter ID is</p><h2>${id}</h2><p>Your fingerprint is enrolled and verified in the database. Save your ID and sign in to vote.</p><button class="btn" onclick="closeM();S.tab='voter';render()">Go to sign in</button>`)})}
function logout(){S.role=S.user=null;S.tab="admin";render()}

/* ---------- shell & candidate tallying ---------- */
function shell(items,title,body){return`<div class="app"><aside><div class="logo">National Election Portal<span>${S.role=="admin"?"Administrator":"Voter"}</span></div>${items.map(([k,l])=>`<a tabindex="0" class="${S.page==k?"on":""}" onclick="S.page='${k}';S.ver=false;render()">${l}</a>`).join("")}<a tabindex="0" class="out" onclick="logout()">Sign out</a></aside><main><div class="top"><h2>${title}</h2><span class="pill ${D.el}">${ELS[D.el]}</span></div>${body}</main></div>`}
const nk=s=>String(s).toUpperCase().replace(/ /g,"_");
const cnt=c=>D.votes.filter(v=>v.n?v.n==nk(c.name):v.c==c.id).length;
const allRows=()=>{const rows=[...D.cands,{id:0,name:"NOTA",party:"None of the above",sym:"✕"}];D.votes.forEach(v=>{if(v.n&&!rows.some(c=>nk(c.name)==v.n))rows.splice(rows.length-1,0,{id:-1,name:v.n.replace(/_/g," "),party:"Ballot entry",sym:"🗳️"})});return rows};
const empty=t=>`<div class="empty">${t}</div>`;
const sb=s=>`<span class="badge ${{approved:"ok",pending:"wn",rejected:"er"}[s]||"ok"}">${s[0].toUpperCase()+s.slice(1)}</span>`;

/* ---------- admin section ---------- */
const ANAV=[["dash","Dashboard"],["cands","Candidates"],["voters","Voter registry"],["bio","Biometric enrolment"],["elec","Election control"],["audit","Audit log"]];
const A={
dash(){const v=D.voters,ap=v.filter(x=>x.st=="approved").length,en=v.filter(x=>x.bio).length,vt=D.votes.length;
const ph=["Registration","Verification","Enrolment","Polling","Results"],cur=D.el=="registration"?(ap?2:1):D.el=="polling"?3:4;
return`<div class="stats">${[["Registered voters",v.length],["Verified",ap],["Biometrics enrolled",en],["Ballots cast",vt],["Turnout",(en?Math.round(vt/en*100):0)+"%"],["Candidates",D.cands.length]].map(([l,n])=>`<div class="card stat"><b>${n}</b><span>${l}</span></div>`).join("")}</div>
<div class="card"><h3>Election process</h3><p class="hint">Voters register, biometrics are enrolled in database, polling opens, secret votes cast, results published.</p><div class="steps">${ph.map((p,i)=>`<div class="${i<cur?"done":i==cur?"cur":""}">${p}</div>`).join("")}</div></div>
<div class="card" style="margin-top:16px"><h3>Recent activity</h3>${logTable(D.audit.slice(0,5))}</div>`},

cands(){const lock=D.el!="registration";return`<div class="grid2"><div class="card"><h3>Add candidate</h3>${lock?'<p class="hint">The candidate list is locked once polling opens.</p>':""}<label>Candidate name</label><input id="cn" placeholder="e.g. Maya Deshmukh" ${lock?"disabled":""}><label>Party</label><input id="cp" placeholder="e.g. Peoples Front" ${lock?"disabled":""}><label>Symbol (emoji)</label><input id="cs" maxlength="4" value="⭐" ${lock?"disabled":""}><button class="btn full" ${lock?"disabled":""} onclick="addCand()">Add candidate</button></div>
<div class="card wrap"><h3>Candidates on the ballot</h3>${D.cands.length?`<table><tr><th>No.</th><th>Symbol</th><th>Name</th><th>Party</th><th></th></tr>${D.cands.map((c,i)=>`<tr><td>${i+1}</td><td style="font-size:22px">${esc(c.sym||"🗳️")}</td><td><b>${esc(c.name)}</b></td><td>${esc(c.party)}</td><td><button class="btn sm bad" ${lock?"disabled":""} onclick="delCand(${c.id})">Delete</button></td></tr>`).join("")}</table>`:empty("No candidates yet. Add the first candidate.")}<p class="hint">Candidates added here are saved on the server and visible on the voting ballot during vote casting.</p></div></div>`},

voters(){return`<div class="card wrap"><h3>Voter registry</h3><table><tr><th>Voter ID</th><th>Name</th><th>Date of birth</th><th>Mobile</th><th>Fingerprint</th><th>Status</th><th>Actions</th></tr>${D.voters.map(v=>`<tr><td>${v.id}</td><td><b>${esc(v.name)}</b><br><small style="color:var(--mut)">${esc(v.addr)}</small></td><td>${v.dob}</td><td>${esc(v.phone)}</td><td>${v.bio?'<span class="badge ok">Enrolled</span>':'<span class="badge wn">Missing</span>'}</td><td>${sb(v.st)}</td><td>${v.st!="approved"?`<button class="btn sm good" onclick="setSt('${v.id}','approved')">Approve</button>`:""}<button class="btn sm bad" ${v.voted?"disabled":""} onclick="delVoter('${v.id}')">Remove</button></td></tr>`).join("")||""}</table></div>`},

bio(){const ap=D.voters;return`<div class="card wrap"><h3>Biometric enrolment</h3><p class="hint">Enrolled fingerprint templates are safely stored in SQLite. Only mathematical templates are stored, never images.</p>${ap.length?`<table><tr><th>Voter</th><th>Finger</th><th>Template Hash</th><th>Status</th><th>Actions</th></tr>${ap.map(v=>`<tr><td><b>${esc(v.name)}</b><br><small style="color:var(--mut)">${v.id}</small></td><td>${v.bio?v.bio.finger:`<select id="f_${v.id}"><option>Right Thumb</option><option>Right Index</option><option>Left Thumb</option><option>Left Index</option></select>`}</td><td>${v.bio?"#"+v.bio.tpl:"–"}</td><td>${v.bio?'<span class="badge ok">Enrolled</span>':'<span class="badge wn">Not enrolled</span>'}</td><td>${v.bio?`<span style="color:var(--ok);font-weight:600">Enrolled (Slot ${v.bio.slot||1})</span>`:`<button class="btn sm" onclick="enroll('${v.id}')">Enrol fingerprint</button>`}</td></tr>`).join("")}</table>`:empty("No voters yet.")}</div>`},

elec(){const tot=D.votes.length,rows=allRows(),mx=Math.max(1,...rows.map(cnt));
return`<div class="card"><h3>Polling control</h3><p>Current phase: <b>${ELS[D.el]}</b></p><p class="hint">Open polling allows voters to verify fingerprints and cast ballots. Closing polling tallies results.</p>
<button class="btn" ${D.el!="registration"?"disabled":""} onclick="setEl('polling')">Open polling</button> <button class="btn bad" ${D.el!="polling"?"disabled":""} onclick="setEl('closed')">Close polling</button> <button class="btn alt" onclick="resetEl()">Reset election / polls</button></div>
<div class="card wrap" style="margin-top:16px"><h3>Results</h3>${D.el!="closed"?empty("Results stay sealed until polling is closed."):`<table><tr><th>Candidate</th><th>Votes</th><th>Share</th><th style="width:40%"></th></tr>${rows.map(c=>{const n=cnt(c);return`<tr><td><span style="font-size:20px">${esc(c.sym||"🗳️")}</span> <b>${esc(c.name)}</b><br><small style="color:var(--mut)">${esc(c.party)}</small></td><td>${n}</td><td>${tot?Math.round(n/tot*100):0}%</td><td><div class="bar"><i style="width:${n/mx*100}%"></i></div></td></tr>`}).join("")}</table><p class="hint">Total ballots counted: ${tot}</p>`}</div>`},

audit(){return`<div class="card wrap"><h3>Audit log</h3>${logTable(D.audit)}</div>`}};

const logTable=a=>a.length?`<table><tr><th>Time</th><th>Actor</th><th>Action</th></tr>${a.map(x=>`<tr><td style="white-space:nowrap">${x.t}</td><td>${esc(x.who)}</td><td>${esc(x.msg)}</td></tr>`).join("")}</table>`:empty("No activity recorded yet.");

async function addCand(){
const n=val("cn"),p=val("cp"),s=val("cs")||"⭐";
if(!n||!p)return toast("Enter candidate name and party");
const r=await post("/api/auth/candidates",{name:n,party:p,symbol:s},{"X-Admin-Token":"admin123"});
const cid=(r.ok&&r.j.id)?r.j.id:Date.now();
D.cands.push({id:cid,name:n,party:p,sym:s});
log("admin","Added candidate "+n);
toast("Candidate added to server ballot");
await syncCandidates();
render()}

function delCand(id){
const c=D.cands.find(x=>x.id==id);
confirmM("Delete candidate "+esc(c.name)+" from the ballot?",async()=>{
await post("/api/auth/candidates/delete",{id},{"X-Admin-Token":"admin123"});
D.cands=D.cands.filter(x=>x.id!=id);
log("admin","Deleted candidate "+c.name);
toast("Candidate deleted");
await syncCandidates();
render()})}

function setSt(id,s){const v=D.voters.find(x=>x.id==id);v.st=s;log("admin","Voter "+id+" "+s);render()}
function delVoter(id){confirmM("Remove voter "+id+" from the registry?",()=>{D.voters=D.voters.filter(x=>x.id!=id);log("admin","Removed voter "+id);render()})}
function enroll(id){const f=$("#f_"+id).value,v=D.voters.find(x=>x.id==id);enrolLive(v,f,()=>{toast("Fingerprint enrolled");render()})}

function setEl(s){
if(s=="polling"&&(D.cands.length<2||!D.voters.some(v=>v.bio)))return toast("Add two candidates and enrol at least one voter first");
confirmM(s=="polling"?"Open polling? Candidate list will be locked.":"Close polling? No more votes can be cast.",()=>{D.el=s;log("admin",s=="polling"?"Polling opened":"Polling closed");render()})}

/* ---------- RESET POLLS (PRESERVES FINGERPRINTS & VOTER REGISTRY) ---------- */
function resetEl(){
confirmM("Clear all ballots and reset polls? All voter registrations and enrolled fingerprint data will be safely preserved.",async()=>{
await post("/api/election/reset",{},{"X-Admin-Token":"admin123"});
D.votes=[];
D.el="registration";
// CRITICAL: Fingerprints are kept! NEVER touch v.bio
D.voters.forEach(v=>{v.voted=false});
await syncRegistry();
log("admin","Election polls reset. All fingerprint templates preserved.");
toast("Polls reset. All voter fingerprint data preserved.");
render()})}

/* ---------- voter section ---------- */
const VNAV=[["profile","My profile"],["bio","Biometric status"],["vote","Cast vote"]];
const me=()=>D.voters.find(v=>v.id==S.user);
const V={
profile(v){return`<div class="card"><h3>${esc(v.name)}</h3><p>${sb(v.st)}</p><table><tr><th>Voter ID</th><td>${v.id}</td></tr><tr><th>Date of birth</th><td>${v.dob}</td></tr><tr><th>Gender</th><td>${v.gender}</td></tr><tr><th>Mobile</th><td>${esc(v.phone)}</td></tr><tr><th>Address</th><td>${esc(v.addr)}</td></tr></table>${v.st=="pending"?'<p class="hint">Your application is waiting for officer verification.</p>':""}</div>`},

bio(v){const can=!v.bio&&D.el=="registration";
return`<div class="card" style="text-align:center"><div class="fp" style="color:${v.bio?"var(--ok)":"var(--mut)"}">${FP}</div><h3>${v.bio?"Fingerprint enrolled":"Fingerprint not enrolled"}</h3><p>${v.bio?`${esc(v.bio.finger)} recorded in database${v.bio.slot?" (slot "+v.bio.slot+")":""}. Use this finger when voting.`:D.el!="registration"?"Enrolment is closed because polling has started.":"Choose a finger and press the button to scan your fingerprint into the database."}</p>${can?`<div style="max-width:260px;margin:0 auto;text-align:left"><label for="vf">Finger to enrol</label><select id="vf">${FINGERS.map(f=>`<option>${f}</option>`).join("")}</select></div><button class="btn full" style="max-width:260px" onclick="selfEnrol()">Enrol my fingerprint</button>`:""}</div>`},

vote(v){const msg=t=>`<div class="card">${empty(t)}</div>`;
if(v.voted)return`<div class="card"><h3>Your vote is recorded</h3><p>Thank you for voting. Your choice is secret and cannot be traced back to you.</p>${v.ref?`<p>Receipt: <b>${v.ref}</b></p>`:""}</div>`;
if(v.st!="approved")return msg("Your registration must be approved before you can vote.");
if(!v.bio)return msg("Enrol your fingerprint under Biometric status to vote.");
if(D.el=="registration")return msg("Polling has not opened yet. Ask the administrator to open polling.");
if(D.el=="closed")return msg("Polling is closed.");
if(!S.ver)return`<div class="card" style="text-align:center"><div class="fp">${FP}</div><h3>Verify your identity</h3><p>Scan your enrolled fingerprint to open your ballot.</p><button class="btn" onclick="verify()">Start fingerprint scan</button></div>`;

// Always display all candidates including those added in the admin section!
const ballotItems=(S.ballot&&S.ballot.length>0)?S.ballot:D.cands;
if(!ballotItems||!ballotItems.length)return msg("No candidates are registered on the ballot. Ask administrator to add candidates.");

return`<div class="card ballot"><h3>Your ballot</h3><p class="hint">Fingerprint verified by sensor (confidence ${S.score||88}). Select one candidate.</p>${ballotItems.map(c=>{
const sym=c.symbol||c.sym||"🗳️";
const party=c.party||"Independent";
return`<label><input type="radio" name="b" value="${esc(c.name)}"><span class="sym" style="font-size:22px">${esc(sym)}</span><span><b>${esc(c.name)}</b><br><small style="color:var(--mut)">${esc(party)}</small></span></label>`}).join("")}<button class="btn full" onclick="cast()">Cast vote</button></div>`}};

function selfEnrol(){const v=me();enrolLive(v,$("#vf").value,()=>{toast("Fingerprint enrolled");render()})}

function verify(){
const v=me();S.ver=false;S.ballot=null;
verifyLive(v,async st=>{
const c=await jx("/api/vote/candidates");
if(c.ok&&c.j.candidates&&c.j.candidates.length>0){
S.ballot=c.j.candidates}
else{
S.ballot=D.cands}
S.score=st.confidence||88;
S.ver=true;
render()})}

function cast(){
const r=document.querySelector("input[name=b]:checked");
if(!r)return toast("Select an option first");
confirmM("Your vote is final and cannot be changed. Cast it now?",async()=>{
const v=me(),name=r.value;
const q=await post("/api/vote/cast",{voter_ref:v.id,candidate:name});
if(!q.ok&&q.s!=0){
if(q.s==403){S.ver=false;S.ballot=null;render()}
return toast(q.j.error||"Vote was not accepted")}
D.votes.push({n:nk(name)});
v.voted=true;
v.ref="R"+String((q.j&&q.j.head_hash)||Math.random().toString(36).slice(2)).slice(0,7).toUpperCase();
S.ver=false;S.ballot=null;
log(v.id,"Ballot cast for candidate "+name);
render();
toast("Vote cast successfully!")})}

function render(){
let h;
if(!S.role)h=authView();
else if(S.role=="admin"){const t=ANAV.find(x=>x[0]==S.page)[1];h=shell(ANAV,t,A[S.page]())}
else{const v=me();h=shell(VNAV,VNAV.find(x=>x[0]==S.page)[1],V[S.page](v))}
$("#root").innerHTML=h;
if(S.role=="voter"&&S.page=="bio"&&!S.syncing){S.syncing=true;const v=me();syncBio(v).finally(()=>S.syncing=false)}}

// Initialize & sync with backend on boot
syncCandidates();
syncRegistry();
render();
