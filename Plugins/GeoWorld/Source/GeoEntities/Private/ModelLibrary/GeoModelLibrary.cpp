#include "ModelLibrary/GeoModelLibrary.h"

#include "GeoCoreModule.h"
#include "Imagery/GeoRuntimeTexture.h"
#include "Models/ModelLoader.h"
#include "Models/SgiImage.h"
#include "Tiles/ImageTileFormat.h"

#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"

using namespace GeoWorld;

namespace
{
	const TCHAR* EntityModelMaterialPaths[3] = {
		TEXT("/GeoWorld/Materials/M_GeoEntity.M_GeoEntity"),
		TEXT("/GeoWorld/Materials/M_GeoEntityMasked.M_GeoEntityMasked"),
		TEXT("/GeoWorld/Materials/M_GeoEntityTranslucent.M_GeoEntityTranslucent") };
	const TCHAR* EntityModelFallbackMaterialPath = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");
	/** Una texture piu' grande di cosi' si riduce (le mip la fanno gia'). */
	constexpr int32 EntityModelMaxTextureSide = 4096;

	std::string ToUtf8(const FString& Text) { return std::string(TCHAR_TO_UTF8(*Text)); }
	FString FromUtf8(const std::string& Text) { return FString(UTF8_TO_TCHAR(Text.c_str())); }

	/** Una sezione del rig in una FMeshDescription (come le forme degli alberi: vertici per triangolo, metri). */
	void FillPartDescription(const Models::FRigPart& Part, FMeshDescription& Out)
	{
		FStaticMeshAttributes Attributes(Out);
		Attributes.Register();
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> Signs = Attributes.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector4f> Colours = Attributes.GetVertexInstanceColors();
		TVertexInstanceAttributesRef<FVector2f> Uvs = Attributes.GetVertexInstanceUVs();
		Uvs.SetNumChannels(1);

		for (int32 SectionIndex = 0; SectionIndex < static_cast<int32>(Part.Sections.size()); ++SectionIndex)
		{
			const Models::FRigSection& Section = Part.Sections[static_cast<size_t>(SectionIndex)];
			const FPolygonGroupID Group = Out.CreatePolygonGroup();
			Attributes.GetPolygonGroupMaterialSlotNames()[Group] = FName(*FString::Printf(TEXT("S%d"), SectionIndex));
			TArray<FVertexInstanceID> Corners;
			Corners.Reserve(static_cast<int32>(Section.Vertices.size()));
			for (const Models::FRigVertex& V : Section.Vertices)
			{
				const FVertexID Vertex = Out.CreateVertex();
				Positions[Vertex] = FVector3f(V.P[0], V.P[1], V.P[2]);
				const FVertexInstanceID Instance = Out.CreateVertexInstance(Vertex);
				const FVector3f Normal(V.N[0], V.N[1], V.N[2]);
				Normals[Instance] = Normal;
				const FVector3f Helper = FMath::Abs(Normal.Z) < 0.9f ? FVector3f::UpVector : FVector3f::ForwardVector;
				Tangents[Instance] = FVector3f::CrossProduct(Helper, Normal).GetSafeNormal();
				Signs[Instance] = 1.0f;
				const FLinearColor Paint = FLinearColor::FromSRGBColor(FColor(V.Rgba[0], V.Rgba[1], V.Rgba[2], V.Rgba[3]));
				Colours[Instance] = FVector4f(Paint.R, Paint.G, Paint.B, Paint.A);
				Uvs.Set(Instance, 0, FVector2f(V.UV[0], V.UV[1]));
				Corners.Add(Instance);
			}
			for (size_t I = 0; I + 2 < Section.Indices.size(); I += 3)
			{
				FVertexInstanceID Corner[3] = {
					Corners[static_cast<int32>(Section.Indices[I])],
					Corners[static_cast<int32>(Section.Indices[I + 1])],
					Corners[static_cast<int32>(Section.Indices[I + 2])] };
				Out.CreateTriangle(Group, MakeArrayView(Corner, 3));
			}
		}
	}
}

Models::FFileAccess FGeoModelLibrary::FileAccess()
{
	Models::FFileAccess Files;
	Files.Read = [](const std::string& Path, std::vector<uint8_t>& Out)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *FromUtf8(Path), FILEREAD_Silent)) { return false; }
		Out.assign(Bytes.GetData(), Bytes.GetData() + Bytes.Num());
		return true;
	};
	Files.Exists = [](const std::string& Path) { return IFileManager::Get().FileExists(*FromUtf8(Path)); };
	return Files;
}

const std::unordered_map<std::string, std::string>* FGeoModelLibrary::BuildIndex(FFolderIndex& FolderIndex)
{
	if (FolderIndex.Folder.IsEmpty()) { return nullptr; }
	// Una volta sola: il primo che arriva lo fa, gli altri aspettano; poi non cambia piu'.
	FScopeLock Lock(&FolderIndex.Lock);
	if (!FolderIndex.bBuilt)
	{
		TArray<FString> Found;
		IFileManager::Get().FindFilesRecursive(Found, *FolderIndex.Folder, TEXT("*"), true, false);
		for (const FString& File : Found)
		{
			FolderIndex.ByName.emplace(Models::LowerCase(ToUtf8(FPaths::GetCleanFilename(File))), Models::NormalisePath(ToUtf8(File)));
		}
		FolderIndex.bBuilt = true;
	}
	return &FolderIndex.ByName;
}

const std::unordered_map<std::string, std::string>* FGeoModelLibrary::GetFolderIndex() const
{
	return Index.IsValid() ? BuildIndex(*Index) : nullptr;
}

FGeoModelLibrary::FGeoModelLibrary()
{
	Completed = MakeShared<FCookedQueue, ESPMode::ThreadSafe>();
	Index = MakeShared<FFolderIndex, ESPMode::ThreadSafe>();
	// NOTA UE: il modulo si carica sul game thread; poi i decodificatori si
	// possono creare da qualsiasi thread.
	Wrappers = &FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
}

FGeoModelLibrary::~FGeoModelLibrary()
{
	for (UE::Tasks::FTask& Task : Tasks) { Task.Wait(); }
	Tasks.Empty();
}

void FGeoModelLibrary::SetFolder(const FString& InFolder)
{
	FString Normalised = InFolder;
	FPaths::NormalizeDirectoryName(Normalised);
	if (!Normalised.IsEmpty() && FPaths::IsRelative(Normalised))
	{
		Normalised = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Normalised);
	}
	Folder = Normalised;
	// Un indice nuovo: quello vecchio lo puo' ancora usare un lavoro in volo.
	Index = MakeShared<FFolderIndex, ESPMode::ThreadSafe>();
	Index->Folder = Folder;
	Clear();
}

FString FGeoModelLibrary::ResolvePath(const FString& File) const
{
	FString Path = File;
	FPaths::NormalizeFilename(Path);
	if (FPaths::IsRelative(Path) && !Folder.IsEmpty()) { Path = FPaths::Combine(Folder, Path); }
	else if (FPaths::IsRelative(Path)) { Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Path); }
	FPaths::NormalizeFilename(Path);
	FPaths::CollapseRelativeDirectories(Path);
	return Path;
}

TSharedPtr<FGeoLoadedModel> FGeoModelLibrary::Request(const FString& Path, const Models::FRigOptions& Options, const FString& Key)
{
	if (TSharedPtr<FGeoLoadedModel>* Found = Loaded.Find(Key)) { return *Found; }
	TSharedPtr<FGeoLoadedModel> Model = MakeShared<FGeoLoadedModel>();
	Model->Key = Key;
	Model->Path = Path;
	Loaded.Add(Key, Model);

	const TSharedPtr<FCookedQueue, ESPMode::ThreadSafe> Queue = Completed;
	const TSharedPtr<FFolderIndex, ESPMode::ThreadSafe> FolderIndex = Index;
	IImageWrapperModule* Decoders = Wrappers;
	const uint32 RequestGeneration = Generation;
	Tasks.Add(UE::Tasks::Launch(TEXT("GeoEntities: modello"), [Key, Path, Options, Queue, FolderIndex, Decoders, RequestGeneration]()
	{
		TSharedPtr<FCooked, ESPMode::ThreadSafe> Result = Cook(Key, Path, Options, FolderIndex, Decoders);
		Result->Generation = RequestGeneration;
		Queue->Enqueue(Result);
	}));
	return Model;
}

TSharedPtr<FGeoModelLibrary::FCooked, ESPMode::ThreadSafe> FGeoModelLibrary::Cook(const FString& Key, const FString& Path,
	const Models::FRigOptions& Options, const TSharedPtr<FFolderIndex, ESPMode::ThreadSafe>& FolderIndex, IImageWrapperModule* Decoders)
{
	const double Start = FPlatformTime::Seconds();
	TSharedPtr<FCooked, ESPMode::ThreadSafe> Out = MakeShared<FCooked, ESPMode::ThreadSafe>();
	Out->Key = Key;

	// L'indice per nome della cartella, una volta sola (il primo lavoro lo fa, gli altri aspettano).
	const std::unordered_map<std::string, std::string>* ByName = FolderIndex.IsValid() ? BuildIndex(*FolderIndex) : nullptr;

	const Models::FFileAccess Files = FileAccess();
	const std::function<bool(const std::string&, std::vector<uint8_t>&)> ReadBytes = Files.Read;
	Models::FModel Model;
	std::string Error;
	Models::FLoadOptions Load;
	Load.TextureIndex = ByName;
	if (!Models::LoadModelFile(ToUtf8(Path), Files, Model, Error, Load))
	{
		Out->Error = FromUtf8(Error);
		Out->Seconds = FPlatformTime::Seconds() - Start;
		return Out;
	}
	Models::FModelRig Rig = Models::BuildRig(Model, Options);
	for (const std::string& Warning : Rig.Warnings) { Out->Warnings.Add(FromUtf8(Warning)); }

	// Le texture: decodificate, in BGRA, con le mipmap.
	for (const Models::FModelTexture& Texture : Rig.Textures)
	{
		FCookedTexture& Cooked = Out->Textures.AddDefaulted_GetRef();
		if (Texture.Resolved.empty()) { Cooked.Error = TEXT("non trovata"); continue; }
		std::vector<uint8_t> Bytes;
		if (!ReadBytes(Texture.Resolved, Bytes)) { Cooked.Error = TEXT("non si legge"); continue; }
		int32 Width = 0, Height = 0;
		std::vector<uint8_t> Bgra;
		if (Models::IsSgiImage(Bytes))
		{
			Models::FDecodedImage Image;
			std::string SgiError;
			if (!Models::DecodeSgiImage(Bytes, Image, SgiError)) { Cooked.Error = FromUtf8(SgiError); continue; }
			Width = Image.Width;
			Height = Image.Height;
			Bgra = std::move(Image.Rgba);
			for (size_t P = 0; P + 3 < Bgra.size(); P += 4) { std::swap(Bgra[P], Bgra[P + 2]); }
		}
		else
		{
			EImageFormat Format = Decoders->DetectImageFormat(Bytes.data(), static_cast<int64>(Bytes.size()));
			// Il TGA non ha una firma all'inizio: se non si riconosce, si prova dall'estensione.
			if (Format == EImageFormat::Invalid && Models::ExtensionOf(Texture.Resolved) == "tga") { Format = EImageFormat::TGA; }
			TSharedPtr<IImageWrapper> Wrapper = Format == EImageFormat::Invalid ? nullptr : Decoders->CreateImageWrapper(Format);
			TArray64<uint8> Raw;
			if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Bytes.data(), static_cast<int64>(Bytes.size()))
				|| !Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw))
			{
				Cooked.Error = TEXT("formato d'immagine che Unreal non decodifica");
				continue;
			}
			Width = static_cast<int32>(Wrapper->GetWidth());
			Height = static_cast<int32>(Wrapper->GetHeight());
			Bgra.assign(Raw.GetData(), Raw.GetData() + Raw.Num());
		}
		if (Width <= 0 || Height <= 0 || Bgra.size() != static_cast<size_t>(Width) * static_cast<size_t>(Height) * 4) { Cooked.Error = TEXT("misure sbagliate"); continue; }
		for (size_t P = 3; P < Bgra.size(); P += 4) { if (Bgra[P] < 255) { Cooked.bHasAlpha = true; break; } }
		std::vector<std::vector<uint8_t>> Mips;
		Tiles::BuildMipChain(Bgra, Width, Height, Mips, true);
		// Troppo grande: si parte dalla prima mip che ci sta.
		while ((Width > EntityModelMaxTextureSide || Height > EntityModelMaxTextureSide) && !Mips.empty())
		{
			Bgra = std::move(Mips.front());
			Mips.erase(Mips.begin());
			Width = FMath::Max(1, Width / 2);
			Height = FMath::Max(1, Height / 2);
		}
		Cooked.bOk = true;
		Cooked.Width = Width;
		Cooked.Height = Height;
		Cooked.Bgra = std::move(Bgra);
		Cooked.Mips = std::move(Mips);
	}
	Out->Rig = MakeShared<const Models::FModelRig, ESPMode::ThreadSafe>(MoveTemp(Rig));
	Out->Seconds = FPlatformTime::Seconds() - Start;
	return Out;
}

int32 FGeoModelLibrary::MaterialMode(const Models::FModelMaterial& Material, bool bTextureHasAlpha)
{
	if (Material.bTranslucent) { return 2; }
	return (Material.bTwoSided || bTextureHasAlpha) ? 1 : 0;
}

void FGeoModelLibrary::EnsureMaterials()
{
	const bool bComplete = BaseMaterials[0].IsValid() && BaseMaterials[1].IsValid() && BaseMaterials[2].IsValid() && !bMaterialsMissing;
	const double Now = FPlatformTime::Seconds();
	if (bComplete || Now - LastMaterialLookup < 2.0) { return; }
	LastMaterialLookup = Now;
	bMaterialsMissing = false;
	UMaterialInterface* Fallback = nullptr;
	for (int32 Mode = 0; Mode < 3; ++Mode)
	{
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, EntityModelMaterialPaths[Mode], nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Material)
		{
			bMaterialsMissing = true;
			if (!Fallback) { Fallback = LoadObject<UMaterialInterface>(nullptr, EntityModelFallbackMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet); }
			Material = Fallback;
		}
		BaseMaterials[Mode].Reset(Material);
	}
}

FString FGeoModelLibrary::GetMaterialProblem() const
{
	return bMaterialsMissing
		? FString(TEXT("Mancano M_GeoEntity (e Masked, Translucent): i modelli sono grigi e senza texture. Lancia geo.Imagery.CreateMaterial nell'editor."))
		: FString();
}

void FGeoModelLibrary::Finish(FCooked& Cooked)
{
	// Un risultato vecchio (un lavoro partito prima di un Clear): si butta,
	// anche se la stessa chiave e' stata richiesta di nuovo nel frattempo.
	if (Cooked.Generation != Generation) { return; }
	TSharedPtr<FGeoLoadedModel>* Found = Loaded.Find(Cooked.Key);
	if (!Found) { return; }
	FGeoLoadedModel& Model = **Found;
	if (Model.State != EGeoModelState::Loading) { return; }
	Model.LoadSeconds = Cooked.Seconds;
	Model.Warnings = Cooked.Warnings;
	if (!Cooked.Error.IsEmpty() || !Cooked.Rig.IsValid())
	{
		Model.State = EGeoModelState::Failed;
		Model.Error = Cooked.Error.IsEmpty() ? FString(TEXT("non letto")) : Cooked.Error;
		UE_LOG(LogGeoWorld, Warning, TEXT("[GeoEntities] modello '%s': %s"), *Model.Path, *Model.Error);
		return;
	}
	EnsureMaterials();
	const Models::FModelRig& Rig = *Cooked.Rig;
	Model.Rig = Cooked.Rig;

	// Le texture: si ripetono (U e V), colori sRGB, con le mipmap del worker.
	for (const FCookedTexture& Texture : Cooked.Textures)
	{
		UTexture2D* Created = Texture.bOk
			? CreateGeoRuntimeDataTexture(Texture.Width, Texture.Height, Texture.Bgra, Texture.Mips, /*bSrgb=*/true, /*bWrapU=*/true, /*bWrapV=*/true)
			: nullptr;
		if (!Created) { ++Model.MissingTextures; }
		Model.Textures.Add(TStrongObjectPtr<UTexture2D>(Created));
	}

	// I materiali: un'istanza dinamica per materiale del rig.
	for (const Models::FModelMaterial& Source : Rig.Materials)
	{
		const bool bTextured = Source.Texture >= 0 && Source.Texture < Model.Textures.Num() && Model.Textures[Source.Texture].IsValid();
		const bool bAlpha = bTextured && Cooked.Textures[Source.Texture].bHasAlpha;
		UMaterialInterface* Base = BaseMaterials[MaterialMode(Source, bAlpha)].Get();
		UMaterialInstanceDynamic* Instance = Base ? UMaterialInstanceDynamic::Create(Base, GetTransientPackage()) : nullptr;
		if (Instance && !bMaterialsMissing)
		{
			if (bTextured) { Instance->SetTextureParameterValue(TEXT("BaseTexture"), Model.Textures[Source.Texture].Get()); }
			Instance->SetVectorParameterValue(TEXT("Emissive"), FLinearColor(Source.Emissive[0], Source.Emissive[1], Source.Emissive[2], 1.0f));
		}
		Model.Materials.Add(TStrongObjectPtr<UMaterialInterface>(Instance));
	}

	// Una mesh per parte.
	for (int32 PartIndex = 0; PartIndex < static_cast<int32>(Rig.Parts.size()); ++PartIndex)
	{
		const Models::FRigPart& Part = Rig.Parts[static_cast<size_t>(PartIndex)];
		if (Part.Sections.empty()) { Model.PartMeshes.Add(TStrongObjectPtr<UStaticMesh>()); continue; }
		FMeshDescription Description;
		FillPartDescription(Part, Description);
		UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
		for (int32 SectionIndex = 0; SectionIndex < static_cast<int32>(Part.Sections.size()); ++SectionIndex)
		{
			const int32 MaterialIndex = Part.Sections[static_cast<size_t>(SectionIndex)].Material;
			UMaterialInterface* Material = Model.Materials.IsValidIndex(MaterialIndex) ? Model.Materials[MaterialIndex].Get() : nullptr;
			Mesh->GetStaticMaterials().Add(FStaticMaterial(Material, FName(*FString::Printf(TEXT("S%d"), SectionIndex))));
		}
		// NOTA UE: come per gli alberi, bFastBuild costruisce i dati di disegno
		// senza la pipeline dell'editor (vale anche nel gioco impacchettato).
		UStaticMesh::FBuildMeshDescriptionsParams BuildParameters;
		BuildParameters.bMarkPackageDirty = false;
		BuildParameters.bBuildSimpleCollision = false;
		BuildParameters.bCommitMeshDescription = false;
		BuildParameters.bFastBuild = true;
		TArray<const FMeshDescription*> Descriptions;
		Descriptions.Add(&Description);
		if (!Mesh->BuildFromMeshDescriptions(Descriptions, BuildParameters))
		{
			Model.Warnings.Add(FString::Printf(TEXT("la parte '%s' non si costruisce"), *FromUtf8(Part.Name)));
			Mesh = nullptr;
		}
		Model.PartMeshes.Add(TStrongObjectPtr<UStaticMesh>(Mesh));
	}
	Model.State = EGeoModelState::Ready;
	UE_LOG(LogGeoWorld, Log, TEXT("[GeoEntities] modello '%s': %d parti, %d triangoli, %d texture (%d mancanti), %.2f s"), *Model.Path,
		static_cast<int32>(Rig.Parts.size()), static_cast<int32>(Rig.TriangleCount()), Model.Textures.Num(), Model.MissingTextures, Model.LoadSeconds);
}

int32 FGeoModelLibrary::Tick()
{
	// Un modello per frame: la costruzione delle mesh sta sul game thread.
	int32 Ready = 0;
	TSharedPtr<FCooked, ESPMode::ThreadSafe> Cooked;
	if (Completed->Dequeue(Cooked) && Cooked.IsValid())
	{
		Finish(*Cooked);
		const TSharedPtr<FGeoLoadedModel>* Found = Loaded.Find(Cooked->Key);
		Ready += (Found && (*Found)->State != EGeoModelState::Loading) ? 1 : 0;
	}
	Tasks.RemoveAll([](const UE::Tasks::FTask& Task) { return Task.IsCompleted(); });
	return Ready;
}

void FGeoModelLibrary::Clear()
{
	Loaded.Empty();
	// I lavori in volo finiscono da soli: quello che consegnano ha la generazione vecchia e si butta.
	++Generation;
	TSharedPtr<FCooked, ESPMode::ThreadSafe> Discard;
	while (Completed->Dequeue(Discard)) {}
}

void FGeoModelLibrary::ForEach(TFunctionRef<void(const FGeoLoadedModel&)> Function) const
{
	for (const TPair<FString, TSharedPtr<FGeoLoadedModel>>& Pair : Loaded) { Function(*Pair.Value); }
}

int32 FGeoModelLibrary::NumLoading() const
{
	int32 Count = 0;
	for (const TPair<FString, TSharedPtr<FGeoLoadedModel>>& Pair : Loaded) { Count += Pair.Value->State == EGeoModelState::Loading ? 1 : 0; }
	return Count;
}
