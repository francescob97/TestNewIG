// =============================================================================
//  ModelLoader.h -- Un modello da un file, qualunque formato. STRATO: C++ PURO.
//
//  Sceglie il lettore dall'estensione. Per i formati che non si leggono
//  direttamente (docs/modelli-design.md, sezione 3) dice come convertirli,
//  invece di un generico "formato non supportato".
// =============================================================================
#pragma once

#include <string>
#include <unordered_map>

#include "Models/ModelData.h"
#include "Models/ModelFiles.h"
#include "Models/ObjReader.h"
#include "Models/OpenFlightReader.h"

namespace GeoWorld::Models
{
	/** Le estensioni che la tabella dei modelli riconosce come file di modello. */
	inline bool IsModelFileExtension(const std::string& Extension)
	{
		static const char* Known[] = { "flt", "obj", "gltf", "glb", "fbx", "ive", "osg", "osgb", "osgt", "3ds", "dae", "lwo", "stl", "ply", "blend" };
		for (const char* K : Known) { if (Extension == K) { return true; } }
		return false;
	}

	/** Si legge adesso (M1): OpenFlight e OBJ. */
	inline bool IsReadableModelExtension(const std::string& Extension) { return Extension == "flt" || Extension == "obj"; }

	/** Per i formati che non si leggono: che cosa fare. */
	inline std::string HowToConvert(const std::string& Extension)
	{
		if (Extension == "gltf" || Extension == "glb") { return "glTF arriva con il secondo pezzo dei modelli (M2)"; }
		if (Extension == "ive" || Extension == "osg" || Extension == "osgb" || Extension == "osgt")
		{
			return "formato interno di OpenSceneGraph: convertilo con 'osgconv modello." + Extension + " modello.flt' (conserva DOF, switch e LOD)";
		}
		if (Extension == "fbx")
		{
			return "FBX e' chiuso (serve l'SDK di Autodesk): convertilo in glTF (FBX2glTF, o Blender) o in OBJ, "
				"oppure importalo nell'editor e usa il suo percorso /Game/...";
		}
		return "convertilo in OBJ o in glTF (Blender, assimp)";
	}

	struct FLoadOptions
	{
		const std::unordered_map<std::string, std::string>* TextureIndex = nullptr;
		int32_t MaxExternalDepth = 8;
	};

	inline bool LoadModelFile(const std::string& Path, const FFileAccess& Files, FModel& Out, std::string& Error,
		const FLoadOptions& Options = FLoadOptions{})
	{
		const std::string Extension = ExtensionOf(Path);
		if (Extension == "flt")
		{
			FOpenFlightOptions Flt;
			Flt.TextureIndex = Options.TextureIndex;
			Flt.MaxExternalDepth = Options.MaxExternalDepth;
			return ReadOpenFlightFile(Path, Files, Out, Error, Flt);
		}
		if (Extension == "obj") { return ReadObj(Path, Files, Out, Error, Options.TextureIndex); }
		Error = "'" + FileNameOf(Path) + "': " + HowToConvert(Extension);
		return false;
	}
}
