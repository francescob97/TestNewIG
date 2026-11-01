#include "GeoEntitySubsystem.h"

#include "GeoCoreModule.h"
#include "GeoEntitySettings.h"
#include "GeoPositionHudSubsystem.h"
#include "Geo/Ellipsoid.h"
#include "Geo/GeoUnits.h"
#include "Georeference/GeoPlaces.h"
#include "Georeference/GeoreferenceSubsystem.h"
#include "Sources/GeoSharedMemorySource.h"
#include "Water/GeoWaterSubsystem.h"

#include "CanvasItem.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Debug/DebugDrawService.h"
#include "DrawDebugHelpers.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SceneInterface.h"
#include "SceneView.h"
#include "UObject/Package.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace GeoWorld;
using namespace GeoWorld::Entities;

namespace
{
	const TCHAR* EntityCubePath = TEXT("/Engine/BasicShapes/Cube.Cube");
	const TCHAR* EntityMaterialPath = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");

	/** Ogni quanto si tolgono quelle che tacciono, e si sceglie chi ha un attore, s. */
	constexpr double EntityExpirePeriodS = 0.5;
	constexpr double EntitySelectPeriodS = 0.25;
	/** Chi ha gia' un attore lo tiene fino a un po' oltre il raggio (niente sfarfallio al bordo). */
	constexpr double EntityRadiusHysteresis = 1.05;
	/** Il nome si scrive un po' sopra l'entita' (in multipli della sua grandezza), e fin dove, m. */
	constexpr double EntityLabelAbove = 0.7;
	constexpr double EntityLabelMaxDistanceM = 50000.0;
	/** La camera che segue: dietro e sopra, in multipli della grandezza dell'entita' piu' un minimo, m. */
	constexpr double EntityFollowBack = 3.0;
	constexpr double EntityFollowBackMinM = 25.0;
	constexpr double EntityFollowUp = 0.8;
	constexpr double EntityFollowUpMinM = 6.0;

	/** I colori delle fazioni: altro giallo, amico blu, avversario rosso, neutrale verde; il ripiego magenta. */
	const FLinearColor EntityForceColours[5] = {
		FLinearColor(0.9f, 0.75f, 0.1f), FLinearColor(0.15f, 0.35f, 0.95f), FLinearColor(0.9f, 0.12f, 0.1f),
		FLinearColor(0.15f, 0.7f, 0.2f), FLinearColor(0.85f, 0.1f, 0.85f) };

	FString ToFString(const std::string& Text) { return FString(UTF8_TO_TCHAR(Text.c_str())); }

	FVector ToVector(const Core::FEcef& V) { return FVector(V.X, V.Y, V.Z); }
}

// ============================================================================
//  Vita
// ============================================================================

bool UGeoEntitySubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) { return false; }
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game
	              || World->WorldType == EWorldType::PIE
	              || World->WorldType == EWorldType::Editor);
}

void UGeoEntitySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UGeoreferenceSubsystem>();
	Queue = MakeShared<FGeoEntityQueue, ESPMode::ThreadSafe>();

	const UGeoEntitySettings* Settings = GetDefault<UGeoEntitySettings>();
	Registry.Settings.BlendSeconds = Settings->BlendSeconds;
	TimeoutSeconds = Settings->TimeoutSeconds;
	ActorRadiusM = Settings->ActorRadiusM;
	MaxActors = Settings->MaxActors;
	bLabels = Settings->bLabels;
	Registry.GeoidUndulation = [this](const Core::FGeodetic& Where) { return UndulationAt(Where); };

	if (!Settings->ModelTable.FilePath.IsEmpty())
	{
		TArray<FString> Errors;
		LoadModelTable(Settings->ModelTable.FilePath, Errors);
		for (const FString& Error : Errors) { UE_LOG(LogGeoWorld, Warning, TEXT("[GeoWorld] Modelli delle entita': %s"), *Error); }
	}

	// I nomi: il motore chiama DrawLabels per ogni viewport, a ogni frame.
	GameDrawHandle = UDebugDrawService::Register(TEXT("Game"), FDebugDrawDelegate::CreateUObject(this, &UGeoEntitySubsystem::DrawLabels));
	EditorDrawHandle = UDebugDrawService::Register(TEXT("Editor"), FDebugDrawDelegate::CreateUObject(this, &UGeoEntitySubsystem::DrawLabels));

	const UWorld* World = GetWorld();
	if (Settings->bListenOnStart && World && World->IsGameWorld())
	{
		FString Error;
		if (!ListenSharedMemory(Settings->SharedMemoryName, Error))
		{
			UE_LOG(LogGeoWorld, Warning, TEXT("[GeoWorld] Entita': %s"), *Error);
		}
	}
}

void UGeoEntitySubsystem::Deinitialize()
{
	// La funzione registrata punta a "this": va tolta prima che l'oggetto sparisca.
	UDebugDrawService::Unregister(GameDrawHandle);
	UDebugDrawService::Unregister(EditorDrawHandle);
	// Prima i thread (non devono piu' toccare la coda), poi gli attori.
	for (TUniquePtr<IGeoEntitySource>& Source : Sources) { Source->Close(); }
	Sources.Empty();
	DestroyAllVisuals();
	Registry.Clear();
	Registry.GeoidUndulation = nullptr;
	Queue.Reset();
	CubeMesh.Reset();
	BaseMaterial.Reset();
	for (TStrongObjectPtr<UMaterialInstanceDynamic>& Material : ForceMaterials) { Material.Reset(); }
	AssetMeshes.Empty();
	Super::Deinitialize();
}

TStatId UGeoEntitySubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGeoEntitySubsystem, STATGROUP_Tickables);
}

// ============================================================================
//  Sorgenti
// ============================================================================

bool UGeoEntitySubsystem::ListenSharedMemory(const FString& MemoryName, FString& OutError)
{
	const FString Name = MemoryName.IsEmpty() ? FString(UTF8_TO_TCHAR(SharedMemory::Name)) : MemoryName;
	for (const TUniquePtr<IGeoEntitySource>& Source : Sources)
	{
		if (Source->GetAddress() == Name) { return true; }
	}
	TUniquePtr<FGeoSharedMemorySource> Source = MakeUnique<FGeoSharedMemorySource>(NextSourceId++, Name);
	if (!Source->Open(Queue.ToSharedRef(), OutError)) { return false; }
	Sources.Add(MoveTemp(Source));
	return true;
}

void UGeoEntitySubsystem::StopAll()
{
	for (TUniquePtr<IGeoEntitySource>& Source : Sources) { Source->Close(); }
	Sources.Empty();
	// Quello che era gia' in coda non serve piu'.
	FEntityUpdate Discard;
	while (Queue.IsValid() && Queue->Dequeue(Discard)) {}
	DestroyAllVisuals();
	Registry.Clear();
	Distances.Empty();
	bFollowing = false;
}

TArray<FString> UGeoEntitySubsystem::DescribeSources() const
{
	TArray<FString> Lines;
	const double Now = FPlatformTime::Seconds();
	for (const TUniquePtr<IGeoEntitySource>& Source : Sources)
	{
		const FGeoEntitySourceStatus Status = Source->GetStatus();
		const TCHAR* State = Status.State == EGeoSourceState::Receiving ? TEXT("ricevo")
			: Status.State == EGeoSourceState::Waiting ? TEXT("in attesa")
			: Status.State == EGeoSourceState::Error ? TEXT("ERRORE") : TEXT("ferma");
		FString Line = FString::Printf(TEXT("%s [%u]: %s - %s"), *Source->GetName(), Source->GetSourceId(), State, *Status.Message);
		if (Status.Messages > 0)
		{
			Line += FString::Printf(TEXT(" (%llu messaggi, l'ultimo %.1f s fa"), Status.Messages, Now - Status.LastMessageSeconds);
			if (Status.Retries > 0) { Line += FString::Printf(TEXT(", %llu letture rifatte"), Status.Retries); }
			Line += TEXT(")");
		}
		if (const FClockSync* Clock = Registry.ClockOf(Source->GetSourceId()))
		{
			Line += FString::Printf(TEXT("; tempo di simulazione %.2f s"), Clock->SimNow(Now));
			if (Clock->ResetCount() > 0) { Line += FString::Printf(TEXT(", ripartito %u volte"), Clock->ResetCount()); }
		}
		Lines.Add(Line);
	}
	return Lines;
}

// ============================================================================
//  Manopole e modelli
// ============================================================================

void UGeoEntitySubsystem::SetBlendSeconds(double InSeconds)
{
	Registry.Settings.BlendSeconds = FMath::Clamp(InSeconds, 0.0, 5.0);
}

void UGeoEntitySubsystem::SetLabelsEnabled(bool bInLabels)
{
	bLabels = bInLabels;
}

bool UGeoEntitySubsystem::LoadModelTable(const FString& Path, TArray<FString>& OutErrors)
{
	FModelTable Table;
	if (!Path.IsEmpty())
	{
		FString Full = Path;
		if (FPaths::IsRelative(Full)) { Full = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Full); }
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Full))
		{
			OutErrors.Add(FString::Printf(TEXT("non riesco a leggere '%s'"), *Full));
			return false;
		}
		std::vector<FModelTableError> Errors;
		Table.Parse(std::string(TCHAR_TO_UTF8(*Text)), Errors);
		for (const FModelTableError& Error : Errors)
		{
			OutErrors.Add(FString::Printf(TEXT("riga %d: %s"), Error.Line, *ToFString(Error.Message)));
		}
		ModelTablePath = Full;
	}
	else
	{
		ModelTablePath.Reset();
	}
	Models = MoveTemp(Table);
	MissingAssets.Reset();
	// Gli attori si rifanno con la tabella nuova (alla prossima scelta).
	++ModelGeneration;
	DestroyAllVisuals();
	NextSelection = 0.0;
	return OutErrors.IsEmpty();
}

double UGeoEntitySubsystem::UndulationAt(const Core::FGeodetic& Where)
{
	const UWorld* World = GetWorld();
	const UGeoWaterSubsystem* Water = World ? World->GetSubsystem<UGeoWaterSubsystem>() : nullptr;
	double Undulation = 0.0;
	if (Water && Water->GetGeoidUndulation(Where.LonRad / Core::DegToRad, Where.LatRad / Core::DegToRad, Undulation))
	{
		return Undulation;
	}
	Stats.bGeoideDiRipiego = true;
	return Places::ItalyGeoidUndulationM;
}

// ============================================================================
//  Il frame
// ============================================================================

void UGeoEntitySubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!Queue.IsValid()) { return; }
	const double Now = FPlatformTime::Seconds();

	DrainQueue(Now);
	if (Now >= NextExpire)
	{
		NextExpire = Now + EntityExpirePeriodS;
		for (const FEntityKey& Key : Registry.Expire(Now, TimeoutSeconds)) { DestroyVisual(Key); }
	}
	if (Now >= NextSelection)
	{
		NextSelection = Now + EntitySelectPeriodS;
		SelectActors(Now);
	}
	UpdateActors(Now);
	UpdateFollow(Now);
	if (bShowDebugOverlay) { DrawDebug(Now); }

	Stats.Entita = static_cast<int32>(Registry.Num());
	Stats.Attori = Visuals.Num();
}

void UGeoEntitySubsystem::DrainQueue(double Now)
{
	FEntityUpdate Update;
	int64 Applied = 0;
	while (Queue->Dequeue(Update))
	{
		const uint64 LateBefore = Registry.Find(Update.Key) ? Registry.Find(Update.Key)->Late : 0;
		const uint32 SnapsBefore = Registry.Find(Update.Key) ? Registry.Find(Update.Key)->Track.Snaps() : 0;
		const EApplyResult Result = Registry.Apply(Update);
		if (Result == EApplyResult::Removed)
		{
			DestroyVisual(Update.Key);
			continue;
		}
		if (const FEntityRecord* Record = Registry.Find(Update.Key))
		{
			Stats.InRitardo += static_cast<int64>(Record->Late - LateBefore);
			Stats.Salti += static_cast<int64>(Record->Track.Snaps() - SnapsBefore);
		}
		++Applied;
	}
	Stats.Aggiornamenti += Applied;
	RateWindowCount += Applied;
	if (Now - RateWindowStart >= 1.0)
	{
		Stats.AggiornamentiAlSecondo = static_cast<float>(RateWindowCount / FMath::Max(Now - RateWindowStart, 1e-3));
		RateWindowStart = Now;
		RateWindowCount = 0;
	}
}

void UGeoEntitySubsystem::SelectActors(double Now)
{
	UWorld* World = GetWorld();
	const UGeoreferenceSubsystem* Georeference = World ? World->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
	FVector Camera = FVector::ZeroVector;
	if (!Georeference || !Georeference->GetActiveViewLocation(Camera)) { return; }
	const Core::FEcef CameraEcef = Georeference->GetSnapshot().UnrealToEcef(Camera);

	// Tutte le entita' del registro, con la loro distanza adesso: i dati
	// costano poco, gli attori molto.
	TArray<FCandidate> Candidates;
	Candidates.Reserve(static_cast<int32>(Registry.Num()));
	Distances.Reset();
	int32 Fallbacks = 0;
	Registry.ForEach([&](const FEntityRecord& Record)
	{
		const FDrawnPose Pose = Registry.PoseAt(Record, Now);
		const double Distance = (Pose.Position - CameraEcef).Length();
		Distances.Add(Record.Key, Distance);
		const double Limit = Visuals.Contains(Record.Key) ? ActorRadiusM * EntityRadiusHysteresis : ActorRadiusM;
		if (Distance <= Limit) { Candidates.Add(FCandidate{ Record.Key, Distance }); }
		if (Models.Find(Record.Kind, Record.ModelId).bFallback) { ++Fallbacks; }
	});
	Stats.Ripieghi = Fallbacks;
	Candidates.Sort([](const FCandidate& A, const FCandidate& B) { return A.DistanceM < B.DistanceM; });
	if (Candidates.Num() > MaxActors) { Candidates.SetNum(MaxActors); }

	TSet<FEntityKey> Keep;
	for (const FCandidate& Candidate : Candidates) { Keep.Add(Candidate.Key); }
	TArray<FEntityKey> Drop;
	for (const TPair<FEntityKey, FVisual>& Pair : Visuals)
	{
		// Via chi e' lontano, e chi e' stato fatto con una tabella dei modelli vecchia.
		if (!Keep.Contains(Pair.Key) || Pair.Value.ModelGeneration != ModelGeneration || !Pair.Value.Actor.IsValid())
		{
			Drop.Add(Pair.Key);
		}
	}
	for (const FEntityKey& Key : Drop) { DestroyVisual(Key); }

	for (const FCandidate& Candidate : Candidates)
	{
		if (FVisual* Existing = Visuals.Find(Candidate.Key))
		{
			// Il nome puo' cambiare (un nominativo nuovo): costa poco rifarlo qui.
			if (const FEntityRecord* Record = Registry.Find(Candidate.Key)) { Existing->LabelText = LabelText(*Record, *Existing); }
			continue;
		}
		const FEntityRecord* Record = Registry.Find(Candidate.Key);
		if (!Record) { continue; }
		FVisual& Visual = Visuals.Add(Candidate.Key);
		if (!SpawnActorFor(*Record, Visual)) { Visuals.Remove(Candidate.Key); }
	}
}

bool UGeoEntitySubsystem::ComputeTransform(const FEntityRecord& Record, FVisual* Visual, double Now,
	FVector& OutLocation, FQuat& OutRotation, Core::FGeodetic& OutGeodetic) const
{
	const UWorld* World = GetWorld();
	const UGeoreferenceSubsystem* Georeference = World ? World->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
	if (!Georeference) { return false; }
	const FGeoreferenceSnapshot Snapshot = Georeference->GetSnapshot();

	const FDrawnPose Pose = Registry.PoseAt(Record, Now);
	Core::FEcef Position = Pose.Position;
	OutGeodetic = Core::EcefToGeodetic(Position);
	if (Record.bClampToGround && Visual)
	{
		// Il terreno disegnato; se la tile non c'e', l'ultima quota nota.
		double Ground = 0.0;
		if (UGeoPositionHudSubsystem::GroundHeightAt(World, OutGeodetic.LonRad / Core::DegToRad, OutGeodetic.LatRad / Core::DegToRad, Ground))
		{
			Visual->GroundM = Ground;
			Visual->bHasGround = true;
		}
		if (Visual->bHasGround)
		{
			OutGeodetic.HeightM = Visual->GroundM;
			Position = Core::GeodeticToEcef(OutGeodetic);
		}
	}

	const Core::FUnrealPos Location = Snapshot.Georeference.EcefToUnreal(Position);
	OutLocation = FVector(Location.X, Location.Y, Location.Z);
	// Gli assi dell'attore direttamente nello spazio di Unreal: le righe sono
	// X, Y, Z (EntityFrames.h, trappola 3). Determinante +1: ToQuat e' lecito.
	const Core::FMat3 Axes = ActorAxesInUnreal(Snapshot.Georeference.GetEcefToUeBasis(), Pose.Orientation);
	const FMatrix Rotation(
		FPlane(Axes.M[0][0], Axes.M[0][1], Axes.M[0][2], 0.0),
		FPlane(Axes.M[1][0], Axes.M[1][1], Axes.M[1][2], 0.0),
		FPlane(Axes.M[2][0], Axes.M[2][1], Axes.M[2][2], 0.0),
		FPlane(0.0, 0.0, 0.0, 1.0));
	OutRotation = Rotation.ToQuat();
	return true;
}

void UGeoEntitySubsystem::UpdateActors(double Now)
{
	for (TPair<FEntityKey, FVisual>& Pair : Visuals)
	{
		AActor* Actor = Pair.Value.Actor.Get();
		const FEntityRecord* Record = Registry.Find(Pair.Key);
		if (!Actor || !Record) { continue; }
		FVector Location;
		FQuat Rotation;
		Core::FGeodetic Where;
		if (!ComputeTransform(*Record, &Pair.Value, Now, Location, Rotation, Where)) { continue; }
		Actor->SetActorLocationAndRotation(Location, Rotation);
	}
}

void UGeoEntitySubsystem::DrawLabels(UCanvas* Canvas, APlayerController* /*Controller*/)
{
	if (!bLabels || Visuals.Num() == 0 || !Canvas || !Canvas->SceneView || !Canvas->SceneView->Family || !GEngine) { return; }
	// Solo i viewport di QUESTO mondo, una volta per vista (come il riquadro della posizione).
	const FSceneInterface* Scene = Canvas->SceneView->Family->Scene;
	if (!Scene || Scene->GetWorld() != GetWorld()) { return; }
	if (LastDrawnFrame == GFrameCounter && LastDrawnView == Canvas->SceneView) { return; }
	LastDrawnFrame = GFrameCounter;
	LastDrawnView = Canvas->SceneView;

	const UFont* Font = GEngine->GetSmallFont();
	if (!Font) { return; }
	const FVector Eye = Canvas->SceneView->ViewLocation;
	const FVector Look = Canvas->SceneView->ViewRotation.Vector();
	for (const TPair<FEntityKey, FVisual>& Pair : Visuals)
	{
		const AActor* Actor = Pair.Value.Actor.Get();
		if (!Actor || Pair.Value.LabelText.IsEmpty()) { continue; }
		const FVector Top = Actor->GetActorLocation() + Actor->GetActorUpVector() * (Pair.Value.ExtentM * EntityLabelAbove * Units::MetersToUu);
		const FVector ToLabel = Top - Eye;
		const double DistanceM = ToLabel.Size() * Units::UuToMeters;
		// Dietro la camera Project darebbe un punto a specchio: si salta.
		if (FVector::DotProduct(ToLabel, Look) <= 0.0 || DistanceM > EntityLabelMaxDistanceM) { continue; }
		const FVector Screen = Canvas->Project(Top);
		const FString Text = bShowDebugOverlay
			? FString::Printf(TEXT("%s  %.1f km"), *Pair.Value.LabelText, DistanceM / 1000.0)
			: Pair.Value.LabelText;
		FCanvasTextItem Item(FVector2D(Screen.X, Screen.Y), FText::FromString(Text), Font,
			Pair.Value.bFallback ? FLinearColor(1.0f, 0.5f, 1.0f) : FLinearColor::White);
		Item.bCentreX = true;
		Item.bCentreY = true;
		Item.EnableShadow(FLinearColor::Black);
		Canvas->DrawItem(Item);
	}
}

FString UGeoEntitySubsystem::LabelText(const FEntityRecord& Record, const FVisual& Visual) const
{
	FString Text = Record.Label.empty() ? FString::Printf(TEXT("#%llu"), Record.Key.Native) : ToFString(Record.Label);
	if (Visual.bFallback) { Text += FString::Printf(TEXT(" ? %s"), *ToFString(Record.Kind.Text())); }
	return Text;
}

// ============================================================================
//  Gli attori
// ============================================================================

UMaterialInterface* UGeoEntitySubsystem::MaterialFor(EForce Force, bool bFallback)
{
	if (!BaseMaterial.IsValid())
	{
		BaseMaterial.Reset(LoadObject<UMaterialInterface>(nullptr, EntityMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet));
		if (!BaseMaterial.IsValid()) { return nullptr; }
	}
	const int32 Slot = bFallback ? 4 : FMath::Clamp(static_cast<int32>(Force), 0, 3);
	if (!ForceMaterials[Slot].IsValid())
	{
		UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(BaseMaterial.Get(), GetTransientPackage());
		// BasicShapeMaterial ha un solo parametro, "Color".
		Material->SetVectorParameterValue(TEXT("Color"), EntityForceColours[Slot]);
		ForceMaterials[Slot].Reset(Material);
	}
	return ForceMaterials[Slot].Get();
}

AActor* UGeoEntitySubsystem::SpawnActorFor(const FEntityRecord& Record, FVisual& Visual)
{
	UWorld* World = GetWorld();
	if (!World) { return nullptr; }
	if (!CubeMesh.IsValid())
	{
		CubeMesh.Reset(LoadObject<UStaticMesh>(nullptr, EntityCubePath, nullptr, LOAD_NoWarn | LOAD_Quiet));
		if (!CubeMesh.IsValid()) { return nullptr; }
	}

	const FModelMatch Match = Models.Find(Record.Kind, Record.ModelId);
	const FModelSpec* Spec = Match.Rule ? &Match.Rule->Model : nullptr;
	Visual.bFallback = Match.bFallback;
	Visual.ModelText = Spec ? ToFString(Spec->Text) : TEXT("box 3 3 3");
	Visual.ModelGeneration = ModelGeneration;

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags |= RF_Transient;
	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), SpawnParameters);
	if (!Actor) { return nullptr; }
	USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	Actor->SetRootComponent(Root);
	Root->RegisterComponent();
#if WITH_EDITOR
	Actor->SetActorLabel(FString::Printf(TEXT("Entita' %s"), *LabelText(Record, Visual)));
#endif

	UMaterialInterface* Material = MaterialFor(Record.Force, Visual.bFallback);
	const auto AddMesh = [&](UStaticMesh* Mesh, const FVector& RelativeLocation, const FVector& Scale)
	{
		UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(Actor);
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetStaticMesh(Mesh);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetupAttachment(Root);
		Component->SetRelativeLocation(RelativeLocation);
		Component->SetRelativeScale3D(Scale);
		Component->RegisterComponent();
		return Component;
	};

	bool bBuilt = false;
	if (Spec && Spec->IsAsset())
	{
		const FString Path = ToFString(Spec->Asset);
		TStrongObjectPtr<UStaticMesh>* Cached = AssetMeshes.Find(Path);
		if (!Cached)
		{
			Cached = &AssetMeshes.Add(Path, TStrongObjectPtr<UStaticMesh>(LoadObject<UStaticMesh>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet)));
			if (!Cached->IsValid()) { MissingAssets.AddUnique(Path); }
		}
		if (Cached->IsValid())
		{
			AddMesh(Cached->Get(), FVector::ZeroVector, FVector::OneVector);
			Visual.ExtentM = FMath::Max(1.0, Cached->Get()->GetBounds().SphereRadius * 2.0 * Units::UuToMeters);
			bBuilt = true;
		}
	}
	if (!bBuilt)
	{
		// Le scatole: il cubo del motore e' di 1 m, la scala e' la misura in metri.
		std::vector<FBoxPart> Parts = Spec && !Spec->IsAsset() ? Spec->Parts : Shapes::Box(3.0, 3.0, 3.0);
		for (const FBoxPart& Part : Parts)
		{
			UStaticMeshComponent* Box = AddMesh(CubeMesh.Get(),
				FVector(Part.CenterX, Part.CenterY, Part.CenterZ) * Units::MetersToUu, FVector(Part.SizeX, Part.SizeY, Part.SizeZ));
			if (Material) { Box->SetMaterial(0, Material); }
		}
		FModelSpec Measure;
		Measure.Parts = Parts;
		Visual.ExtentM = Measure.Extent();
	}

	Visual.LabelText = LabelText(Record, Visual);
	Visual.Actor = Actor;
	return Actor;
}

void UGeoEntitySubsystem::DestroyVisual(const FEntityKey& Key)
{
	FVisual Visual;
	if (!Visuals.RemoveAndCopyValue(Key, Visual)) { return; }
	if (AActor* Actor = Visual.Actor.Get()) { Actor->Destroy(); }
}

void UGeoEntitySubsystem::DestroyAllVisuals()
{
	for (TPair<FEntityKey, FVisual>& Pair : Visuals)
	{
		if (AActor* Actor = Pair.Value.Actor.Get()) { Actor->Destroy(); }
	}
	Visuals.Empty();
}

// ============================================================================
//  Seguire
// ============================================================================

bool UGeoEntitySubsystem::Follow(const FString& What, FString& OutDescription)
{
	const FString Wanted = What.TrimStartAndEnd();
	const bool bNumber = Wanted.IsNumeric();
	const uint64 Number = bNumber ? FCString::Strtoui64(*Wanted, nullptr, 10) : 0;
	const FEntityRecord* Best = nullptr;
	double BestDistance = TNumericLimits<double>::Max();
	Registry.ForEach([&](const FEntityRecord& Record)
	{
		const bool bMatch = bNumber ? Record.Key.Native == Number : ToFString(Record.Label).Equals(Wanted, ESearchCase::IgnoreCase);
		if (!bMatch) { return; }
		const double* Distance = Distances.Find(Record.Key);
		const double D = Distance ? *Distance : 0.0;
		if (!Best || D < BestDistance) { Best = &Record; BestDistance = D; }
	});
	if (!Best) { return false; }
	bFollowing = true;
	FollowedKey = Best->Key;
	FollowedLabel = Best->Label.empty() ? FString::Printf(TEXT("#%llu"), Best->Key.Native) : ToFString(Best->Label);
	OutDescription = FollowedLabel;
	return true;
}

void UGeoEntitySubsystem::UpdateFollow(double Now)
{
	if (!bFollowing) { return; }
	const FEntityRecord* Record = Registry.Find(FollowedKey);
	UWorld* World = GetWorld();
	APlayerController* Player = World ? World->GetFirstPlayerController() : nullptr;
	APawn* Pawn = Player ? Player->GetPawn() : nullptr;
	if (!Record || !Pawn)
	{
		if (!Record) { bFollowing = false; }
		return;
	}
	FVector Location;
	FQuat Rotation;
	Core::FGeodetic Where;
	FVisual* Visual = Visuals.Find(FollowedKey);
	if (!ComputeTransform(*Record, Visual, Now, Location, Rotation, Where)) { return; }

	// Dietro (secondo la rotta, non il muso: un aereo che rolla non fa
	// rollare la camera) e sopra, guardandola.
	const UGeoreferenceSubsystem* Georeference = World->GetSubsystem<UGeoreferenceSubsystem>();
	if (!Georeference) { return; }
	const Core::FEcef UpEcef = Core::GeodeticSurfaceNormal(Where);
	const FVector Up = ToVector(Georeference->GetSnapshot().Georeference.GetEcefToUeBasis().Transform(UpEcef)).GetSafeNormal();
	FVector Forward = Rotation.GetAxisX();
	Forward = (Forward - Up * FVector::DotProduct(Forward, Up)).GetSafeNormal();
	if (Forward.IsNearlyZero()) { Forward = Rotation.GetAxisZ(); }
	const double Extent = Visual ? Visual->ExtentM : 5.0;
	const double Back = FMath::Max(Extent * EntityFollowBack, EntityFollowBackMinM);
	const double Above = FMath::Max(Extent * EntityFollowUp, EntityFollowUpMinM);
	const FVector Eye = Location - Forward * (Back * Units::MetersToUu) + Up * (Above * Units::MetersToUu);
	Pawn->SetActorLocation(Eye);
	Player->SetControlRotation((Location - Eye).Rotation());
}

// ============================================================================
//  Diagnostica
// ============================================================================

void UGeoEntitySubsystem::DrawDebug(double Now) const
{
	UWorld* World = GetWorld();
	const UGeoreferenceSubsystem* Georeference = World ? World->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
	if (!Georeference) { return; }
	const FGeoreferenceSnapshot Snapshot = Georeference->GetSnapshot();
	const Core::FMat3& Basis = Snapshot.Georeference.GetEcefToUeBasis();
	for (const TPair<FEntityKey, FVisual>& Pair : Visuals)
	{
		const FEntityRecord* Record = Registry.Find(Pair.Key);
		const AActor* Actor = Pair.Value.Actor.Get();
		if (!Record || !Actor) { continue; }
		const FDrawnPose Pose = Registry.PoseAt(*Record, Now);
		const FVector From = Actor->GetActorLocation();
		// Dove sara' fra un secondo (verde), e se c'e' un rientro in corso dove
		// dice l'ultimo aggiornamento (giallo): la distanza e' l'errore che si
		// sta smaltendo. Rosso: non arrivano aggiornamenti da troppo.
		const FVector Velocity = ToVector(Basis.Transform(Pose.Velocity)) * Units::MetersToUu;
		DrawDebugLine(World, From, From + Velocity, Pose.bStale ? FColor::Red : FColor::Green, false, -1.0f, 0, 3.0f);
		if (Pose.BlendRemaining > 0.0)
		{
			const Core::FUnrealPos Truth = Snapshot.Georeference.EcefToUnreal(Pose.TruthPosition);
			DrawDebugLine(World, From, FVector(Truth.X, Truth.Y, Truth.Z), FColor::Yellow, false, -1.0f, 0, 2.0f);
		}
	}
}

TArray<FString> UGeoEntitySubsystem::DescribeEntities(int32 MaxLines) const
{
	TArray<const FEntityRecord*> All;
	Registry.ForEach([&](const FEntityRecord& Record) { All.Add(&Record); });
	All.Sort([this](const FEntityRecord& A, const FEntityRecord& B)
	{
		const double* Da = Distances.Find(A.Key);
		const double* Db = Distances.Find(B.Key);
		return (Da ? *Da : 1e30) < (Db ? *Db : 1e30);
	});

	TArray<FString> Lines;
	const double Now = FPlatformTime::Seconds();
	for (const FEntityRecord* Record : All)
	{
		if (Lines.Num() >= MaxLines) { break; }
		const FDrawnPose Pose = Registry.PoseAt(*Record, Now);
		const Core::FGeodetic Where = Core::EcefToGeodetic(Pose.Position);
		const double Lat = Where.LatRad / Core::DegToRad, Lon = Where.LonRad / Core::DegToRad;
		const FEcef Ned = EcefToNed(Where.LatRad, Where.LonRad, Pose.Velocity);
		const double Course = FMath::RadiansToDegrees(std::atan2(Ned.Y, Ned.X));
		const FModelMatch Model = Models.Find(Record->Kind, Record->ModelId);
		const double* Distance = Distances.Find(Record->Key);
		const FVisual* Visual = Visuals.Find(Record->Key);
		FString Label = Record->Label.empty() ? FString::Printf(TEXT("#%llu"), Record->Key.Native) : ToFString(Record->Label);
		Lines.Add(FString::Printf(
			TEXT("  %-12s %-17s %s%s  %.5f %.5f  %5.0f m%s  %5.1f m/s rotta %3.0f  %s  a %.1f km%s  %.1f s fa"),
			*Label, *ToFString(Record->Kind.Text()),
			Model.Rule ? *ToFString(Model.Rule->Model.Text) : TEXT("box 3 3 3"), Model.bFallback ? TEXT(" (ripiego)") : TEXT(""),
			Lat, Lon, Where.HeightM, Record->bClampToGround ? TEXT(" (al suolo)") : TEXT(""),
			Pose.Velocity.Length(), Course < 0.0 ? Course + 360.0 : Course,
			UTF8_TO_TCHAR(DeadReckoningName(Record->Track.Algorithm())),
			Distance ? *Distance / 1000.0 : -1.0, Visual ? TEXT(", a schermo") : TEXT(""),
			Now - Record->LastHeardSeconds));
	}
	return Lines;
}

FString UGeoEntitySubsystem::DescribeForHud() const
{
	if (Sources.Num() == 0 && Registry.Num() == 0) { return FString(); }
	FString Text = FString::Printf(TEXT("%d, %d a schermo"), static_cast<int32>(Registry.Num()), Visuals.Num());
	if (Stats.Ripieghi > 0) { Text += FString::Printf(TEXT(", %d con la forma di ripiego"), Stats.Ripieghi); }
	for (const TUniquePtr<IGeoEntitySource>& Source : Sources)
	{
		const EGeoSourceState State = Source->GetStatus().State;
		Text += State == EGeoSourceState::Receiving ? TEXT("; ricevo")
			: State == EGeoSourceState::Waiting ? TEXT("; aspetto la sorgente")
			: State == EGeoSourceState::Error ? TEXT("; ERRORE (geo.Entities.List)") : TEXT("; ferma");
	}
	return Text;
}

FString UGeoEntitySubsystem::DescribeFollowed() const
{
	if (!bFollowing) { return FString(); }
	const FEntityRecord* Record = Registry.Find(FollowedKey);
	if (!Record) { return FString(); }
	const FDrawnPose Pose = Registry.PoseAt(*Record, FPlatformTime::Seconds());
	const Core::FGeodetic Where = Core::EcefToGeodetic(Pose.Position);
	return FString::Printf(TEXT("%s, %.0f m/s (%.0f km/h), quota %.0f m"), *FollowedLabel, Pose.Velocity.Length(),
		Pose.Velocity.Length() * 3.6, Where.HeightM);
}
