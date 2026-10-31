"""Exercise the production CMake identities with independently changed inputs."""
from pathlib import Path
import subprocess
import tempfile
import shutil
import re

root=Path(__file__).resolve().parents[1]
module=root/'runtime/nvn/vulkan/shader_identity.cmake'
with tempfile.TemporaryDirectory(prefix='totk-shader-identities-') as tmp:
    fixture=Path(tmp)
    names=set(re.findall(r'\b(?:runtime|tools)/[^\s;)]+\.(?:c|h|cpp|hpp|inc|py)\b',module.read_text()))
    for name in names:
        target=fixture/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(root/name,target)
    script=fixture/'check.cmake'
    script.write_text(f'include("{module}")\ntotk_shader_identity("{fixture}")\n'
                      'file(WRITE "'+str(fixture/'ids')+'" "${SHADER_TRANSLATION_ID}\n${SHADER_PREPARE_ID}\n${SHADER_PLAN_ID}\n${PIPELINE_RECIPE_ID}\n")\n')
    def identities():
        subprocess.run(['cmake','-P',str(script)],check=True,capture_output=True)
        return (fixture/'ids').read_text().splitlines()
    baseline=identities();assert len(baseline)==4 and all(re.fullmatch('[a-f0-9]{64}',x) for x in baseline)
    for name in ['exec.c','compile_budget.c','shader_module_cache.c','shader_artifact_storage.cpp','shader_artifact_storage.hpp','shader_warm.cpp','descriptor_update.c','vk_command_worker.c','pass_clear.c','pipeline_warm.c']:
        target=fixture/'runtime/nvn/vulkan'/name;target.write_text('unrelated renderer or scheduling change\n')
        assert identities()==baseline,name
    cases=[('runtime/nvn/maxwell/translate.c',{0,1}),('runtime/nvn/maxwell/shader.h',{0,1}),
           ('runtime/nvn/vulkan/shader_config.c',{0,1}),('runtime/nvn/vulkan/shader_keys.c',{0,1}),
           ('runtime/nvn/vulkan/shader_source_key.h',{0,1}),
           ('runtime/nvn/vulkan/shader_artifact.h',{0,1}),('runtime/nvn/vulkan/shader_warm_plan.hpp',{1,2}),
           ('runtime/nvn/vulkan/shader_prepare.cpp',{1}),('runtime/nvn/vulkan/pipeline_state.h',{3}),
           ('runtime/nvn/vulkan/pipeline_seed.c',{3})]
    for name,expected in cases:
        target=fixture/name;before=target.read_bytes();target.write_bytes(before+b'\nchanged semantic input\n')
        changed=identities();assert {i for i in range(4) if changed[i]!=baseline[i]}==expected,name
        target.write_bytes(before)
    assert identities()==baseline
print('PASS: unrelated renderer/scheduling changes retain mappings; translation/interface/key, coverage and recipe ABI invalidate only their contracts')
