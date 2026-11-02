"""Exercise the browser's actual profile and template checks without a DOM."""
from pathlib import Path
import shutil
import subprocess
import unittest


@unittest.skipUnless(shutil.which('node'),'Node is required to execute browser profile validation')
class LivingV12FrontendTests(unittest.TestCase):
    def test_both_living_profiles_enable_every_owned_template_and_reject_wrong_sources(self):
        script=Path(__file__).resolve().parents[1]/'app/static/studio.js'
        program=r'''
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const source=fs.readFileSync(process.argv[1],'utf8');
const extract=name=>{
  const start=source.indexOf(`function ${name}(`);
  assert(start>=0,`Missing browser validator ${name}`);
  const next=source.indexOf('\nfunction ',start+1);
  return source.slice(start,next<0?source.length:next);
};
const context={catalog:{},current:{},targetStatus:()=>({eligible:true}),bytes:s=>Buffer.byteLength(s,'utf8')};
vm.createContext(context);
vm.runInContext(['supportsTemplates','residentProfile','templateStatus'].map(extract).join('\n'),context);
for(const profile of ['living-v11','living-v12']) {
  context.catalog.target_registry={profile};
  assert.strictEqual(context.supportsTemplates(),true);
  assert.strictEqual(context.residentProfile(),true);
  for(const spawn of [902000,902011,902016,902022,902025,902035,902044]) {
    const base=spawn>=902035?38:11328;
    context.current={card:{spawn_id:String(spawn),map_id:725,name:'Obyvatel'},authoring:{role:'Obyvatel'}};
    context.catalog.targets=[{spawn_id:String(spawn),entry:base}];
    const result=context.templateStatus();
    assert.strictEqual(result.eligible,true,`${profile}/${spawn}`);
    assert.strictEqual(result.entry,9810000+spawn-902000);
    assert.strictEqual(result.base_entry,base);
    context.catalog.targets[0].entry=base===38?11328:38;
    assert.strictEqual(context.templateStatus().eligible,false);
  }
  context.current.card.spawn_id='902045';
  context.catalog.targets=[{spawn_id:'902045',entry:38}];
  assert.strictEqual(context.templateStatus().eligible,false);
}
'''
        result=subprocess.run(['node','-e',program,str(script)],capture_output=True,text=True,timeout=20)
        self.assertEqual(result.returncode,0,result.stderr[-3000:])


if __name__=='__main__':unittest.main()
