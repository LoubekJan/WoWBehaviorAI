"""V12 deploy adoption and production inputs retain the living V11 behavior."""
from pathlib import Path
import json
import shutil
import subprocess
import sys
import tempfile
import unittest

from tools.realm_lab.tests import test_authoring_integration as authoring
from tools.realm_lab.tests import test_living_v11_integration as previous
from tools.realm_lab.tests import test_living_v11_runtime_mounts as mounts


class LivingV12RuntimeMountTests(unittest.TestCase):
    def test_actual_v12_loader_uses_only_the_existing_readonly_runtime_inputs(self):
        blocks=mounts.consumer_blocks()
        needed=('tools/realm_lab','data/realm_lab',mounts.HEADER)
        baseline=tuple(b for b in mounts.workspace_binds(blocks['worldserver']) if b[1] in needed)
        for service in mounts.CONSUMERS:
            self.assertEqual(tuple(b for b in mounts.workspace_binds(blocks[service]) if b[1] in needed),baseline)
        with tempfile.TemporaryDirectory(prefix='living-v12-runtime-') as folder:
            root=Path(folder).resolve()
            for source,target,mode in baseline:
                self.assertEqual(mode,'ro')
                destination=root/target
                self.assertTrue(destination.resolve().is_relative_to(root))
                original=mounts.ROOT/source
                if original.is_dir():shutil.copytree(original,destination,ignore=shutil.ignore_patterns('__pycache__','tests'))
                else:
                    destination.parent.mkdir(parents=True,exist_ok=True)
                    shutil.copyfile(original,destination)
            program='''import json,sys
from pathlib import Path
sys.path.insert(0,sys.argv[1])
from tools.realm_lab import living_v12
p=living_v12.Population.load()
assert living_v12.LOCATIONS_HEADER == Path(sys.argv[1])/sys.argv[2]
assert p.config()['AIWorld.LivingResidentsV11.Enabled']=='1'
print(json.dumps(dict(profile=p.profile,source_revision=p.source_revision,actors=len(p.actors))))
'''
            result=subprocess.run([sys.executable,'-I','-c',program,str(root),mounts.HEADER],cwd=root,capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stderr[-3000:])
            self.assertEqual(json.loads(result.stdout),dict(profile='living-v12',source_revision='preview-v12',actors=45))


@unittest.skipUnless(authoring.bash_binary(),'Bash is required for deployment-flow checks')
class LivingV12ShellTests(unittest.TestCase):
    HARNESS=previous.LivingShellIntegrationTests.HARNESS.replace('preflight-living-v11-adoption','preflight-living-v12-adoption')
    run_script=authoring.AuthoringShellIntegrationTests.run_script

    def test_map_upgrade_requires_all45_existing_bindings_before_stopping_the_world(self):
        for filename in ('setup-world.sh','deploy-lab.sh'):
            with self.subTest(script=filename):
                result,calls=self.run_script(filename,profile='living-v12')
                self.assertEqual(result.returncode,0,result.stderr[-2000:])
                order=[next(i for i,c in enumerate(calls) if token in c) for token in
                    ('preflight-living-v12-adoption','character_templates plan','stop worldserver',
                     'character_templates restore','world_setup.py prepare','bootstrap-living-v12','character_templates apply')]
                self.assertEqual(order,sorted(order))
                self.assertFalse(any('bootstrap-settlement-v8' in c or 'bootstrap-full-v8' in c for c in calls))

    def test_failed_geometry_or_adoption_cannot_stop_or_repopulate_the_world(self):
        for filename in ('setup-world.sh','deploy-lab.sh'):
            with self.subTest(script=filename):
                result,calls=self.run_script(filename,profile='living-v12',fail=True)
                self.assertEqual(result.returncode,42,result.stderr[-2000:])
                self.assertFalse(any('stop worldserver' in c or 'bootstrap-' in c for c in calls))


if __name__=='__main__':unittest.main()
