"""Runtime services must receive every input used by their population loader.

The build service mounts the entire checkout; production services deliberately
mount only their read-only inputs. A successful tc-dev preflight alone cannot
prove that a production process can load the same geometry contract.
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT=Path(__file__).resolve().parents[3]
CONSUMERS=('worldserver','world-viewer','character-studio','aiworld-recorder')
HEADER='src/server/game/AIWorld/Agent/LivingResidentsV11Locations.h'
HEADER_BIND=f'./{HEADER}:/workspace/{HEADER}:ro'


def consumer_blocks():
    source=(ROOT/'compose.yml').read_text(encoding='utf-8')
    services=source.split('\nservices:\n',1)[1].split('\nvolumes:\n',1)[0]
    return dict((m.group(1),m.group(2)) for m in re.finditer(
        r'^  ([a-z0-9-]+):\n(.*?)(?=^  [a-z0-9-]+:\n|\Z)',services,re.M|re.S))


def workspace_binds(block):
    values=[]
    for value in re.findall(r'^      - (.+)$',block,re.M):
        if value.startswith('./') and ':/workspace/' in value:
            source,target,mode=value.rsplit(':',2)
            values.append((source[2:],target[len('/workspace/'):],mode))
    return values


class LivingRuntimeMountTests(unittest.TestCase):
    def test_each_partial_runtime_service_has_the_exact_readonly_header_binding(self):
        blocks=consumer_blocks()
        owners={name for name,block in blocks.items() if HEADER_BIND in block}
        self.assertEqual(owners,set(CONSUMERS))
        for name in CONSUMERS:
            with self.subTest(service=name):
                binds=workspace_binds(blocks[name])
                self.assertIn((HEADER,HEADER,'ro'),binds)
                self.assertIn(('tools/realm_lab','tools/realm_lab','ro'),binds)
                self.assertIn(('data/realm_lab','data/realm_lab','ro'),binds)
                self.assertNotIn('.:/workspace',blocks[name])

    def test_actual_loader_runs_in_each_declared_partial_tree_and_refuses_missing_header(self):
        blocks=consumer_blocks()
        with tempfile.TemporaryDirectory(prefix='living-runtime-inputs-') as directory:
            temporary=Path(directory).resolve()
            for service in CONSUMERS:
                with self.subTest(service=service):
                    workspace=temporary/service
                    workspace.mkdir()
                    # Copy only the three actual read-only mounts required by
                    # Population.load, never the unrepresentative whole checkout.
                    binds=[b for b in workspace_binds(blocks[service]) if b[1] in
                           ('tools/realm_lab','data/realm_lab',HEADER)]
                    for source,target,mode in binds:
                        self.assertEqual(mode,'ro')
                        destination=workspace/target
                        self.assertTrue(destination.resolve().is_relative_to(temporary))
                        original=ROOT/source
                        if original.is_dir():
                            shutil.copytree(original,destination,ignore=shutil.ignore_patterns('__pycache__','tests'))
                        else:
                            destination.parent.mkdir(parents=True,exist_ok=True)
                            shutil.copyfile(original,destination)
                    program='''import json,sys
from pathlib import Path
sys.path.insert(0,sys.argv[1])
from tools.realm_lab import living_v11
p=living_v11.Population.load()
assert living_v11.LOCATIONS_HEADER == Path(sys.argv[1]) / sys.argv[2]
print(json.dumps({'actors':len(p.actors),'profile':p.profile,'routes':p.positions['native_navigation']['complete_paths']}))
'''
                    command=[sys.executable,'-I','-c',program,str(workspace),HEADER]
                    result=subprocess.run(command,cwd=workspace,capture_output=True,text=True,timeout=30)
                    self.assertEqual(result.returncode,0,result.stderr[-2000:])
                    self.assertEqual(json.loads(result.stdout),dict(actors=45,profile='living-v11',routes=346))
                    header=workspace/HEADER
                    header.unlink()
                    missing=subprocess.run(command,cwd=workspace,capture_output=True,text=True,timeout=30)
                    self.assertNotEqual(missing.returncode,0)
                    self.assertIn('LivingResidentsV11Locations.h',missing.stderr)

    @unittest.skipUnless(shutil.which('docker'),'Docker Compose CLI is required to validate its normalized model')
    def test_compose_normalized_mounts_are_readonly_files_for_all_four_consumers(self):
        result=subprocess.run(['docker','compose','--profile','*','--env-file',str(ROOT/'.env.example'),'-f',str(ROOT/'compose.yml'),
            'config','--format','json'],cwd=ROOT,env=dict(os.environ),capture_output=True,text=True,timeout=30)
        self.assertEqual(result.returncode,0,result.stderr[-2000:])
        services=json.loads(result.stdout)['services']
        for name in CONSUMERS:
            matches=[v for v in services[name]['volumes'] if v['target']=='/workspace/'+HEADER]
            with self.subTest(service=name):
                self.assertEqual(len(matches),1)
                self.assertEqual(matches[0]['type'],'bind')
                self.assertIs(matches[0]['read_only'],True)
                self.assertEqual(Path(matches[0]['source']).resolve(),(ROOT/HEADER).resolve())


if __name__=='__main__':unittest.main()
