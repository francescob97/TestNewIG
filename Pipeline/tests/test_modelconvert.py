"""
Test di geoworld convert-models (docs/modelli-design.md, M2) con strumenti FINTI:
tre script Python che si comportano come osgconv, FBX2glTF e Blender (stessi
argomenti, stesso file scritto). Cosi' si prova tutto quello che e' nostro: quale
strumento per quale formato, dove va il file convertito, il "make" (quelli gia'
aggiornati si saltano), le copie con --output, lo strumento che manca, lo
strumento che fallisce, le texture che il .flt nomina e non ci sono.

Gli strumenti veri si provano a mano (docs/modelli-verifica.md).
"""

import os
import shutil
import struct
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from geoworld import cli, modelconvert as mc  # noqa: E402

FAKE_OSGCONV = r'''
import struct, sys
source, target = sys.argv[1], sys.argv[2]
if "rotto" in source:
    print("Error reading file " + source + ": file not handled")
    sys.exit(1)
# Un .flt minimo: l'intestazione (finta) e una palette delle texture che nomina mimetica.rgb.
name = b"D:\\lavoro\\texture\\mimetica.rgb"
record = struct.pack(">HH", 64, 216) + name + b"\0" * (200 - len(name)) + b"\0" * 12
with open(target, "wb") as handle:
    handle.write(struct.pack(">HH", 1, 8) + b"\0" * 4 + record)
'''

FAKE_FBX2GLTF = r'''
import sys
args = sys.argv[1:]
assert "--binary" in args
output = args[args.index("--output") + 1]
with open(output + ".glb", "wb") as handle:
    handle.write(b"glTF")
'''

FAKE_BLENDER = r'''
import sys
args = sys.argv[1:]
assert "--background" in args
script = args[args.index("--python") + 1]
with open(script, encoding="utf-8") as handle:
    assert "export_scene.gltf" in handle.read()
source, target = args[args.index("--") + 1:][:2]
with open(target, "wb") as handle:
    handle.write(b"glTF")
'''


def write(path, data=b"x"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(data)


class ConvertModelsTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="geoworld-models-")
        self.tools_dir = os.path.join(self.root, "strumenti")
        os.makedirs(self.tools_dir)
        self.fake = {}
        for name, text in (("osgconv", FAKE_OSGCONV), ("fbx2gltf", FAKE_FBX2GLTF), ("blender", FAKE_BLENDER)):
            path = os.path.join(self.tools_dir, f"finto_{name}.py")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(text)
            self.fake[name] = path
        self.src = os.path.join(self.root, "modelli")
        write(os.path.join(self.src, "carro.ive"))
        write(os.path.join(self.src, "aerei", "f16.fbx"))
        write(os.path.join(self.src, "aerei", "f16.png"))
        write(os.path.join(self.src, "edifici", "torre.dae"))
        write(os.path.join(self.src, "edifici", "casa.flt"))
        self.messages = []

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def tools(self, **missing):
        return mc.Tools(osgconv=None if missing.get("osgconv") else self.fake["osgconv"],
                        fbx2gltf=None if missing.get("fbx2gltf") else self.fake["fbx2gltf"],
                        blender=None if missing.get("blender") else self.fake["blender"])

    def run_convert(self, **kwargs):
        tools = kwargs.pop("tools", None) or self.tools()
        return mc.convert(self.src, tools=tools, report=self.messages.append, **kwargs)

    def test_plan_chooses_tool_and_target(self):
        jobs, copies = mc.plan(self.src, None, self.tools())
        found = {job.relative.replace("\\", "/"): (job.tool, os.path.relpath(job.target, self.src).replace("\\", "/")) for job in jobs}
        self.assertEqual(found, {
            "carro.ive": ("osgconv", "carro.flt"),
            "aerei/f16.fbx": ("fbx2gltf", "aerei/f16.glb"),
            "edifici/torre.dae": ("blender", "edifici/torre.glb"),
        })
        self.assertEqual(copies, [], "senza --output non si copia niente: si scrive accanto agli originali")
        jobs, _ = mc.plan(self.src, None, self.tools(fbx2gltf=True))
        self.assertEqual({job.relative.replace("\\", "/"): job.tool for job in jobs}["aerei/f16.fbx"], "blender",
                         "senza FBX2glTF, l'FBX passa da Blender")
        jobs, _ = mc.plan(self.src, None, self.tools(), osg_format="obj")
        self.assertTrue(any(job.target.endswith("carro.obj") for job in jobs))

    def test_convert_in_place_then_up_to_date(self):
        result = self.run_convert()
        self.assertTrue(result.ok, self.messages)
        self.assertEqual(len(result.converted), 3)
        for path in ("carro.flt", "aerei/f16.glb", "edifici/torre.glb"):
            self.assertTrue(os.path.isfile(os.path.join(self.src, path)), path)
        again = self.run_convert()
        self.assertEqual((len(again.converted), len(again.up_to_date)), (0, 3), "la seconda volta sono tutti aggiornati")
        # Un sorgente cambiato dopo la conversione si riconverte.
        time.sleep(0.05)
        future = time.time() + 10
        os.utime(os.path.join(self.src, "carro.ive"), (future, future))
        changed = self.run_convert()
        self.assertEqual(changed.converted, ["carro.ive"])
        forced = self.run_convert(force=True)
        self.assertEqual(len(forced.converted), 3, "--force riconverte tutto")

    def test_missing_texture_reported_then_found(self):
        result = self.run_convert()
        self.assertEqual(result.missing_textures, [("carro.ive", ["mimetica.rgb"])],
                         "il .flt nomina mimetica.rgb (con il percorso di un'altra macchina) e non c'e'")
        write(os.path.join(self.src, "mimetica.rgb"))
        found = self.run_convert(force=True)
        self.assertEqual(found.missing_textures, [], "nella cartella dei modelli si trova per nome")

    def test_output_mirror_copies_textures_and_readable(self):
        out = os.path.join(self.root, "convertiti")
        result = self.run_convert(output_dir=out)
        self.assertTrue(result.ok, self.messages)
        for path in ("carro.flt", "aerei/f16.glb", "edifici/torre.glb", "aerei/f16.png", "edifici/casa.flt"):
            self.assertTrue(os.path.isfile(os.path.join(out, path)), path)
        self.assertEqual(result.copied, 2, "la texture e il .flt che si legge gia'")
        self.assertFalse(os.path.exists(os.path.join(self.src, "carro.flt")), "gli originali non si toccano")
        again = self.run_convert(output_dir=out)
        self.assertEqual((again.copied, len(again.converted)), (0, 0))

    def test_output_inside_source_is_not_reconverted(self):
        out = os.path.join(self.src, "convertiti")
        self.run_convert(output_dir=out)
        write(os.path.join(out, "vecchio.ive"))
        jobs, _ = mc.plan(self.src, out, self.tools())
        self.assertFalse(any("convertiti" in job.relative for job in jobs), "quello che si e' scritto non si riprende")

    def test_missing_tool_does_not_stop_the_rest(self):
        result = self.run_convert(tools=self.tools(osgconv=True))
        self.assertFalse(result.ok)
        self.assertEqual(result.missing_tool, [("carro.ive", "osgconv")])
        self.assertEqual(len(result.converted), 2)
        self.assertTrue(any(message.startswith("MANCA osgconv") for message in self.messages), self.messages)

    def test_failing_tool_is_reported(self):
        write(os.path.join(self.src, "rotto.ive"))
        result = self.run_convert()
        self.assertFalse(result.ok)
        self.assertEqual(len(result.failed), 1)
        relative, reason = result.failed[0]
        self.assertEqual(relative, "rotto.ive")
        self.assertIn("file not handled", reason, "il motivo e' l'ultima riga dello strumento")
        self.assertEqual(len(result.converted), 3)

    def test_dry_run_writes_nothing(self):
        out = os.path.join(self.root, "convertiti")
        result = self.run_convert(output_dir=out, dry_run=True)
        self.assertEqual(len(result.converted), 3)
        self.assertFalse(os.path.exists(out))

    def test_cli(self):
        code = cli.main(["convert-models", self.src, "--dry-run", "--osgconv", self.fake["osgconv"],
                         "--fbx2gltf", self.fake["fbx2gltf"], "--blender", self.fake["blender"]])
        self.assertEqual(code, 0)
        self.assertEqual(cli.main(["convert-models", os.path.join(self.root, "non-c-e")]), 1)


class FltAndScriptTest(unittest.TestCase):
    def test_flt_texture_names(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "x.flt")
            names = [b"tex/a.rgb", b"C:\\db\\b.png"]
            data = struct.pack(">HH", 1, 8) + b"\0" * 4
            for name in names:
                data += struct.pack(">HH", 64, 216) + name + b"\0" * (212 - len(name))
            data += struct.pack(">HH", 2, 4)
            with open(path, "wb") as handle:
                handle.write(data)
            self.assertEqual(mc.flt_texture_names(path), ["tex/a.rgb", "C:\\db\\b.png"])
            write(os.path.join(folder, "a.rgb"))
            self.assertEqual(mc.missing_textures(path, []), ["b.png"], "a.rgb e' accanto al modello (per nome)")

    def test_blender_script(self):
        script = mc.BLENDER_SCRIPT
        self.assertIn('export_format="GLB"', script)
        self.assertIn("export_animations=True", script)
        self.assertIn("export_draco_mesh_compression_enable=False", script, "senza Draco: GeoWorld non la legge")
        for extension in (".fbx", ".dae", ".3ds", ".stl", ".ply", ".abc", ".usd", ".x3d"):
            self.assertIn(f'"{extension}"', script)
        compile(script, "geoworld_blender_export.py", "exec")


if __name__ == "__main__":
    unittest.main()
