#include "GeoEntitySubsystem.h"

#include "GeoCoreModule.h"
#include "GeoEntitySettings.h"
#include "GeoPositionHudSubsystem.h"
#include "Geo/Ellipsoid.h"
#include "Geo/GeoUnits.h"
#include "Georeference/GeoPlaces.h"
#include "Georeference/GeoreferenceSubsystem.h"
#include "ModelLibrary/GeoModelLibrary.h"
#include "Models/ModelLoader.h"
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

	/**
	 * Da una trasformazione del rig (colonne = immagini degli assi) a una
	 * FTransform. NOTA UE: FMatrix e' "a vettore riga": le righe sono le
	 * immagini degli assi e l'ultima la traslazione.
	 */
	FTransform AffineToTransform(const GeoWorld::Models::FAffine& A)
	{
		FMatrix M = FMatrix::Identity;
		for (int32 Row = 0; Row < 3; ++Row)
		{
			for (int32 Column = 0; Column < 3; ++Column) { M.M[Row][Column] = A.M[Column][Row]; }
			M.M[3][Row] = A.T[Row];
		}
		return FTransform(M);
	}

	/** La forma di serie del dominio (mentre il modello da file si legge, o se non si legge). */
	std::vector<FBoxPart> DefaultShape(const FEntityKind& Kind)
	{
		static const FModelTable BuiltIns;
		const FModelMatch Match = BuiltIns.Find(Kind);
		return Match.Rule ? Match.Rule->Model.Parts : Shapes::Box(3.0, 3.0, 3.0);
	}
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
	Library = MakeShared<FGeoModelLibrary>();
	Library->SetFolder(Settings->ModelFolder.Path);

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
	Library.Reset();
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
	// Un modello da file appena pronto: alla prossima scelta le entita' che lo aspettano lo ricevono.
	if (Library.IsValid() && Library->Tick() > 0) { NextSelection = 0.0; }
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
		// E chi aspettava il suo modello da file, ora che e' pronto (o che non si legge).
		const bool bModelArrived = Pair.Value.bWaitingModel && Pair.Value.FileModel.IsValid() && Pair.Value.FileModel->State != EGeoModelState::Loading;
		if (!Keep.Contains(Pair.Key) || Pair.Value.ModelGeneration != ModelGeneration || !Pair.Value.Actor.IsValid() || bModelArrived)
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

	// I modelli da file: switch e LOD secondo la camera di adesso.
	for (TPair<FEntityKey, FVisual>& Pair : Visuals)
	{
		if (Pair.Value.Parts.Num() > 0) { ApplyRig(Pair.Value, &Camera); }
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
		if (Pair.Value.Animation >= 0) { AnimateVisual(Pair.Value, Now); }
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
	if (Visual.bWaitingModel) { Text += TEXT(" (modello in lettura)"); }
	else if (Visual.FileModel.IsValid() && Visual.FileModel->State == EGeoModelState::Failed) { Text += TEXT(" (modello non letto)"); }
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

	// Un modello da file: lo chiede alla libreria (la chiave e' il file piu' le correzioni).
	if (Spec && Spec->IsFile() && Library.IsValid())
	{
		const FString Path = Library->ResolvePath(ToFString(Spec->File));
		Visual.FileModel = Library->Request(Path, Spec->FileOptions, Path + TEXT("|") + ToFString(Spec->Text));
		Visual.AutoAnimation = ToFString(Spec->Animation);
		Visual.bWaitingModel = Visual.FileModel->State == EGeoModelState::Loading;
		// Non si legge: la forma di serie, in magenta, come un ripiego.
		if (Visual.FileModel->State == EGeoModelState::Failed) { Visual.bFallback = true; }
	}

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
	if (Visual.FileModel.IsValid() && Visual.FileModel->State == EGeoModelState::Ready && Visual.FileModel->Rig.IsValid())
	{
		BuildModelParts(Actor, Root, *Visual.FileModel, Visual);
		bBuilt = true;
		// L'animazione della regola ('anim'): in loop, ognuno a un punto diverso del giro (non tutti i radar in fase).
		const GeoWorld::Models::FModelRig& Rig = *Visual.FileModel->Rig;
		if (!Visual.AutoAnimation.IsEmpty())
		{
			const int32 Found = Rig.FindAnimation(std::string(TCHAR_TO_UTF8(*Visual.AutoAnimation)));
			if (Found >= 0)
			{
				const double Duration = Rig.Animations[static_cast<size_t>(Found)].Duration;
				Visual.Animation = Found;
				Visual.bAnimationLoop = true;
				Visual.AnimationSpeed = 1.0;
				Visual.AnimationStart = FPlatformTime::Seconds() - Duration * static_cast<double>(GetTypeHash(Record.Key) % 1000u) / 1000.0;
			}
			else
			{
				const FString Problem = FString::Printf(TEXT("%s: nessuna animazione '%s' (geo.Entities.ModelInfo per l'elenco)"),
					*Visual.FileModel->Path, *Visual.AutoAnimation);
				if (!ReportedProblems.Contains(Problem))
				{
					ReportedProblems.Add(Problem);
					UE_LOG(LogGeoWorld, Warning, TEXT("[GeoEntities] %s"), *Problem);
				}
			}
		}
	}
	else if (Spec && Spec->IsAsset())
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
		std::vector<FBoxPart> Parts = Spec && !Spec->IsAsset() && !Spec->IsFile() ? Spec->Parts : DefaultShape(Record.Kind);
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
	const int32 Loading = Library.IsValid() ? Library->NumLoading() : 0;
	if (Loading > 0) { Text += FString::Printf(TEXT("; %d modelli in lettura"), Loading); }
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

// ============================================================================
//  I modelli da file (docs/modelli-design.md)
// ============================================================================

void UGeoEntitySubsystem::BuildModelParts(AActor* Actor, USceneComponent* Root, const FGeoLoadedModel& Model, FVisual& Visual)
{
	const GeoWorld::Models::FModelRig& Rig = *Model.Rig;
	// Il rig e' in metri: un componente scalato di 100 sotto la radice, e sotto
	// le parti con le loro trasformazioni in metri, come le da' il rig.
	USceneComponent* ModelRoot = NewObject<USceneComponent>(Actor, TEXT("Modello"));
	ModelRoot->SetMobility(EComponentMobility::Movable);
	ModelRoot->SetupAttachment(Root);
	ModelRoot->SetRelativeScale3D(FVector(Units::MetersToUu));
	ModelRoot->RegisterComponent();

	Visual.RigState = GeoWorld::Models::DefaultRigState(Rig);
	Visual.Parts.Reset();
	for (int32 Index = 0; Index < static_cast<int32>(Rig.Parts.size()); ++Index)
	{
		const GeoWorld::Models::FRigPart& Part = Rig.Parts[static_cast<size_t>(Index)];
		// Le madri vengono sempre prima delle figlie (il rig e' in ordine di visita).
		USceneComponent* Parent = ModelRoot;
		if (Part.Parent >= 0 && Visual.Parts.IsValidIndex(Part.Parent) && Visual.Parts[Part.Parent].IsValid()) { Parent = Visual.Parts[Part.Parent].Get(); }
		UStaticMesh* Mesh = Model.PartMeshes.IsValidIndex(Index) ? Model.PartMeshes[Index].Get() : nullptr;
		USceneComponent* Component = nullptr;
		if (Mesh)
		{
			UStaticMeshComponent* MeshComponent = NewObject<UStaticMeshComponent>(Actor);
			MeshComponent->SetStaticMesh(Mesh);
			MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Component = MeshComponent;
		}
		else
		{
			Component = NewObject<USceneComponent>(Actor);
		}
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetupAttachment(Parent);
		Component->SetRelativeTransform(AffineToTransform(GeoWorld::Models::PartTransform(Part, Visual.RigState, static_cast<size_t>(Index))));
		Component->RegisterComponent();
		Visual.Parts.Add(Component);
	}
	Visual.ExtentM = Rig.Extent();
	// La visibilita' (switch, LOD) la decide la scelta degli attori subito dopo, con la camera.
	ApplyRig(Visual, nullptr);
}

void UGeoEntitySubsystem::ApplyRig(FVisual& Visual, const FVector* Camera) const
{
	const AActor* Actor = Visual.Actor.Get();
	if (!Actor || !Visual.FileModel.IsValid() || !Visual.FileModel->Rig.IsValid()) { return; }
	const GeoWorld::Models::FModelRig& Rig = *Visual.FileModel->Rig;
	// La camera nel sistema della radice del modello (metri): la distanza dei LOD.
	double CameraRoot[3] = { 0.0, 0.0, 0.0 };
	if (Camera)
	{
		const FVector Local = Actor->GetActorTransform().InverseTransformPosition(*Camera) * Units::UuToMeters;
		CameraRoot[0] = Local.X; CameraRoot[1] = Local.Y; CameraRoot[2] = Local.Z;
	}
	for (int32 Index = 0; Index < Visual.Parts.Num() && Index < static_cast<int32>(Rig.Parts.size()); ++Index)
	{
		USceneComponent* Component = Visual.Parts[Index].Get();
		if (!Component) { continue; }
		const GeoWorld::Models::FRigPart& Part = Rig.Parts[static_cast<size_t>(Index)];
		if (Part.Kind != GeoWorld::Models::EPartKind::Static)
		{
			Component->SetRelativeTransform(AffineToTransform(GeoWorld::Models::PartTransform(Part, Visual.RigState, static_cast<size_t>(Index))));
		}
		if (Camera && (!Part.Switches.empty() || !Part.Lods.empty()))
		{
			const bool bVisible = GeoWorld::Models::IsPartVisible(Rig, Part, Visual.RigState, CameraRoot);
			if (Component->IsVisible() != bVisible) { Component->SetVisibility(bVisible, false); }
		}
	}
}

const UGeoEntitySubsystem::FVisual* UGeoEntitySubsystem::FindVisual(const FString& Who, FString& OutLabel) const
{
	const FString Wanted = Who.TrimStartAndEnd();
	const bool bNumber = Wanted.IsNumeric();
	const uint64 Number = bNumber ? FCString::Strtoui64(*Wanted, nullptr, 10) : 0;
	const FVisual* Best = nullptr;
	double BestDistance = TNumericLimits<double>::Max();
	Registry.ForEach([&](const FEntityRecord& Record)
	{
		const FString Label = Record.Label.empty() ? FString::Printf(TEXT("#%llu"), Record.Key.Native) : ToFString(Record.Label);
		const bool bMatch = bNumber ? Record.Key.Native == Number : Label.Equals(Wanted, ESearchCase::IgnoreCase);
		const FVisual* Visual = bMatch ? Visuals.Find(Record.Key) : nullptr;
		if (!Visual) { return; }
		const double* Distance = Distances.Find(Record.Key);
		const double D = Distance ? *Distance : 0.0;
		if (!Best || D < BestDistance) { Best = Visual; BestDistance = D; OutLabel = Label; }
	});
	return Best;
}

UGeoEntitySubsystem::FVisual* UGeoEntitySubsystem::FindVisual(const FString& Who, FString& OutLabel)
{
	return const_cast<FVisual*>(static_cast<const UGeoEntitySubsystem*>(this)->FindVisual(Who, OutLabel));
}

void UGeoEntitySubsystem::SetModelFolder(const FString& Folder)
{
	if (!Library.IsValid()) { return; }
	Library->SetFolder(Folder);
	ReloadModels();
}

FString UGeoEntitySubsystem::GetModelFolder() const
{
	return Library.IsValid() ? Library->GetFolder() : FString();
}

void UGeoEntitySubsystem::ReloadModels()
{
	// Prima gli attori (tengono i modelli), poi la libreria.
	DestroyAllVisuals();
	if (Library.IsValid()) { Library->Clear(); }
	NextSelection = 0.0;
}

FString UGeoEntitySubsystem::GetModelMaterialProblem() const
{
	return Library.IsValid() ? Library->GetMaterialProblem() : FString();
}

TArray<FString> UGeoEntitySubsystem::DescribeFileModels() const
{
	TArray<FString> Lines;
	if (!Library.IsValid()) { return Lines; }
	Library->ForEach([&Lines](const FGeoLoadedModel& Model)
	{
		if (Model.State == EGeoModelState::Loading)
		{
			Lines.Add(FString::Printf(TEXT("  %s: in lettura"), *Model.Path));
		}
		else if (Model.State == EGeoModelState::Failed)
		{
			Lines.Add(FString::Printf(TEXT("  %s: NON LETTO - %s"), *Model.Path, *Model.Error));
		}
		else
		{
			const GeoWorld::Models::FModelRig& Rig = *Model.Rig;
			Lines.Add(FString::Printf(TEXT("  %s: %s, %d parti (%d DOF, %d animate, %d switch), %d animazioni, %d triangoli, lungo %.1f m, %d texture%s, %.2f s"),
				*Model.Path, UTF8_TO_TCHAR(Rig.Format.c_str()), static_cast<int32>(Rig.Parts.size()), static_cast<int32>(Rig.DofCount()),
				static_cast<int32>(Rig.AnimatedCount()), static_cast<int32>(Rig.Switches.size()), static_cast<int32>(Rig.Animations.size()),
				static_cast<int32>(Rig.TriangleCount()), Rig.Extent(), Model.Textures.Num(),
				Model.MissingTextures > 0 ? *FString::Printf(TEXT(" (%d mancanti)"), Model.MissingTextures) : TEXT(""), Model.LoadSeconds));
		}
		for (const FString& Warning : Model.Warnings) { Lines.Add(TEXT("      avviso: ") + Warning); }
	});
	return Lines;
}

bool UGeoEntitySubsystem::InspectModelFile(const FString& File, TArray<FString>& OutLines) const
{
	if (!Library.IsValid()) { return false; }
	const FString Path = Library->ResolvePath(File);
	GeoWorld::Models::FLoadOptions Load;
	Load.TextureIndex = Library->GetFolderIndex();
	GeoWorld::Models::FModel Model;
	std::string Error;
	if (!GeoWorld::Models::LoadModelFile(std::string(TCHAR_TO_UTF8(*Path)), FGeoModelLibrary::FileAccess(), Model, Error, Load))
	{
		OutLines.Add(FString::Printf(TEXT("%s: %s"), *Path, *ToFString(Error)));
		return false;
	}
	const GeoWorld::Models::FModelRig Rig = GeoWorld::Models::BuildRig(Model);
	OutLines.Add(FString::Printf(TEXT("%s: %s, %d nodi, %d triangoli, %d materiali, %d punti luce"), *Path, *ToFString(Model.Format),
		static_cast<int32>(Model.Nodes.size()), static_cast<int32>(Model.TriangleCount()), static_cast<int32>(Model.Materials.size()),
		static_cast<int32>(Model.LightPoints.size())));
	OutLines.Add(FString::Printf(TEXT("  misure (avanti, destra, alto): %.2f x %.2f x %.2f m; %d parti, %d DOF, %d animate, %d switch/sequenze"),
		Rig.BoundsMax[0] - Rig.BoundsMin[0], Rig.BoundsMax[1] - Rig.BoundsMin[1], Rig.BoundsMax[2] - Rig.BoundsMin[2],
		static_cast<int32>(Rig.Parts.size()), static_cast<int32>(Rig.DofCount()), static_cast<int32>(Rig.AnimatedCount()),
		static_cast<int32>(Rig.Switches.size())));
	for (const GeoWorld::Models::FRigPart& Part : Rig.Parts)
	{
		if (Part.Kind != GeoWorld::Models::EPartKind::Dof) { continue; }
		const GeoWorld::Models::FDof& D = Part.Dof;
		OutLines.Add(FString::Printf(TEXT("  DOF '%s': imbardata %.0f..%.0f, beccheggio %.0f..%.0f, rollio %.0f..%.0f"), *ToFString(Part.Name),
			D.Rotate[2].Min, D.Rotate[2].Max, D.Rotate[0].Min, D.Rotate[0].Max, D.Rotate[1].Min, D.Rotate[1].Max));
	}
	for (const GeoWorld::Models::FRigSwitch& Switch : Rig.Switches)
	{
		OutLines.Add(FString::Printf(TEXT("  %s '%s': %d figli, %d maschere"), Switch.bSequence ? TEXT("sequenza") : TEXT("switch"),
			*ToFString(Switch.Name), Switch.ChildCount, Switch.MaskCount()));
	}
	for (const GeoWorld::Models::FModelTexture& Texture : Model.Textures)
	{
		const FString Where = !Texture.Embedded.empty() ? FString::Printf(TEXT("dentro il file (%d byte)"), static_cast<int32>(Texture.Embedded.size()))
			: Texture.Resolved.empty() ? FString(TEXT("NON TROVATA")) : ToFString(Texture.Resolved);
		OutLines.Add(FString::Printf(TEXT("  texture %s: %s"), *ToFString(Texture.Path), *Where));
	}
	for (size_t Index = 0; Index < Rig.Animations.size(); ++Index)
	{
		const GeoWorld::Models::FRigAnimation& Animation = Rig.Animations[Index];
		OutLines.Add(FString::Printf(TEXT("  animazione %d '%s': %.2f s, %d canali"), static_cast<int32>(Index), *ToFString(Animation.Name),
			Animation.Duration, static_cast<int32>(Animation.Channels.size())));
	}
	for (const std::string& Warning : Model.Warnings) { OutLines.Add(TEXT("  avviso: ") + ToFString(Warning)); }
	return true;
}

bool UGeoEntitySubsystem::DescribeEntityRig(const FString& Who, TArray<FString>& OutLines) const
{
	FString Label;
	const FVisual* Visual = FindVisual(Who, Label);
	if (!Visual) { OutLines.Add(FString::Printf(TEXT("'%s': nessuna entita' a schermo con questo nome"), *Who)); return false; }
	if (!Visual->FileModel.IsValid() || !Visual->FileModel->Rig.IsValid())
	{
		OutLines.Add(FString::Printf(TEXT("%s: non ha un modello da file (%s)"), *Label, *Visual->ModelText));
		return false;
	}
	const GeoWorld::Models::FModelRig& Rig = *Visual->FileModel->Rig;
	OutLines.Add(FString::Printf(TEXT("%s: %s"), *Label, *Visual->FileModel->Path));
	for (size_t Index = 0; Index < Rig.Parts.size(); ++Index)
	{
		const GeoWorld::Models::FRigPart& Part = Rig.Parts[Index];
		if (Part.Kind != GeoWorld::Models::EPartKind::Dof || Index >= Visual->RigState.Dofs.size()) { continue; }
		const GeoWorld::Models::FDofValues& V = Visual->RigState.Dofs[Index];
		const GeoWorld::Models::FDof& D = Part.Dof;
		OutLines.Add(FString::Printf(TEXT("  DOF '%s': imbardata %.1f (%.0f..%.0f), beccheggio %.1f (%.0f..%.0f), rollio %.1f (%.0f..%.0f)"),
			*ToFString(Part.Name), V.Rotate[2], D.Rotate[2].Min, D.Rotate[2].Max, V.Rotate[0], D.Rotate[0].Min, D.Rotate[0].Max,
			V.Rotate[1], D.Rotate[1].Min, D.Rotate[1].Max));
	}
	for (size_t Index = 0; Index < Rig.Switches.size(); ++Index)
	{
		const GeoWorld::Models::FRigSwitch& Switch = Rig.Switches[Index];
		const int32 Mask = Index < Visual->RigState.Masks.size() ? Visual->RigState.Masks[Index] : 0;
		OutLines.Add(FString::Printf(TEXT("  %s '%s': maschera %d di %d"), Switch.bSequence ? TEXT("sequenza") : TEXT("switch"),
			*ToFString(Switch.Name), Mask, Switch.MaskCount()));
	}
	for (size_t Index = 0; Index < Rig.Animations.size(); ++Index)
	{
		const GeoWorld::Models::FRigAnimation& Animation = Rig.Animations[Index];
		const bool bPlaying = Visual->Animation == static_cast<int32>(Index);
		OutLines.Add(FString::Printf(TEXT("  animazione %d '%s': %.2f s%s"), static_cast<int32>(Index), *ToFString(Animation.Name), Animation.Duration,
			bPlaying ? (Visual->bAnimationLoop ? TEXT(" (gira, in loop)") : TEXT(" (gira)")) : TEXT("")));
	}
	return true;
}

bool UGeoEntitySubsystem::SetEntityDof(const FString& Who, const FString& Dof, const TArray<double>& Values, FString& OutMessage)
{
	FString Label;
	FVisual* Visual = FindVisual(Who, Label);
	if (!Visual || !Visual->FileModel.IsValid() || !Visual->FileModel->Rig.IsValid())
	{
		OutMessage = FString::Printf(TEXT("'%s': nessuna entita' a schermo con un modello da file"), *Who);
		return false;
	}
	const GeoWorld::Models::FModelRig& Rig = *Visual->FileModel->Rig;
	int32 Found = -1;
	for (size_t Index = 0; Index < Rig.Parts.size(); ++Index)
	{
		if (Rig.Parts[Index].Kind == GeoWorld::Models::EPartKind::Dof && ToFString(Rig.Parts[Index].Name).Equals(Dof, ESearchCase::IgnoreCase))
		{
			Found = static_cast<int32>(Index);
			break;
		}
	}
	if (Found < 0 || static_cast<size_t>(Found) >= Visual->RigState.Dofs.size())
	{
		OutMessage = FString::Printf(TEXT("%s: nessun DOF '%s' (geo.Entities.Dof %s per l'elenco)"), *Label, *Dof, *Who);
		return false;
	}
	GeoWorld::Models::FDofValues V = Visual->RigState.Dofs[static_cast<size_t>(Found)];
	// L'ordine dei valori: imbardata, beccheggio, rollio (gradi), poi x, y, z (m) nel sistema del DOF.
	if (Values.Num() >= 1) { V.Rotate[2] = Values[0]; }
	if (Values.Num() >= 2) { V.Rotate[0] = Values[1]; }
	if (Values.Num() >= 3) { V.Rotate[1] = Values[2]; }
	for (int32 Axis = 0; Axis < 3; ++Axis) { if (Values.Num() >= 4 + Axis) { V.Translate[Axis] = Values[3 + Axis]; } }
	const GeoWorld::Models::FRigPart& Part = Rig.Parts[static_cast<size_t>(Found)];
	V = GeoWorld::Models::ClampDof(Part.Dof, V);
	Visual->RigState.Dofs[static_cast<size_t>(Found)] = V;
	ApplyRig(*Visual, nullptr);
	OutMessage = FString::Printf(TEXT("%s, DOF '%s': imbardata %.1f, beccheggio %.1f, rollio %.1f (dentro i limiti del modello)"),
		*Label, *ToFString(Part.Name), V.Rotate[2], V.Rotate[0], V.Rotate[1]);
	return true;
}

bool UGeoEntitySubsystem::SetEntitySwitch(const FString& Who, const FString& Switch, int32 Mask, FString& OutMessage)
{
	FString Label;
	FVisual* Visual = FindVisual(Who, Label);
	if (!Visual || !Visual->FileModel.IsValid() || !Visual->FileModel->Rig.IsValid())
	{
		OutMessage = FString::Printf(TEXT("'%s': nessuna entita' a schermo con un modello da file"), *Who);
		return false;
	}
	const GeoWorld::Models::FModelRig& Rig = *Visual->FileModel->Rig;
	for (size_t Index = 0; Index < Rig.Switches.size() && Index < Visual->RigState.Masks.size(); ++Index)
	{
		if (!ToFString(Rig.Switches[Index].Name).Equals(Switch, ESearchCase::IgnoreCase)) { continue; }
		const int32 Count = Rig.Switches[Index].MaskCount();
		Visual->RigState.Masks[Index] = FMath::Clamp(Mask, 0, FMath::Max(0, Count - 1));
		UWorld* World = GetWorld();
		const UGeoreferenceSubsystem* Georeference = World ? World->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
		FVector Camera = FVector::ZeroVector;
		// Senza camera i LOD si guardano dall'origine del modello (da vicino).
		if (!Georeference || !Georeference->GetActiveViewLocation(Camera)) { Camera = Visual->Actor.IsValid() ? Visual->Actor->GetActorLocation() : FVector::ZeroVector; }
		ApplyRig(*Visual, &Camera);
		OutMessage = FString::Printf(TEXT("%s, switch '%s': maschera %d di %d"), *Label, *Switch, Visual->RigState.Masks[Index], Count);
		return true;
	}
	OutMessage = FString::Printf(TEXT("%s: nessuno switch '%s' (geo.Entities.Dof %s per l'elenco)"), *Label, *Switch, *Who);
	return false;
}

void UGeoEntitySubsystem::AnimateVisual(FVisual& Visual, double Now) const
{
	if (!Visual.FileModel.IsValid() || !Visual.FileModel->Rig.IsValid()) { Visual.Animation = -1; return; }
	const GeoWorld::Models::FModelRig& Rig = *Visual.FileModel->Rig;
	if (Visual.Animation < 0 || static_cast<size_t>(Visual.Animation) >= Rig.Animations.size()) { Visual.Animation = -1; return; }
	const double Duration = Rig.Animations[static_cast<size_t>(Visual.Animation)].Duration;
	// In avanti si parte dal primo fotogramma, all'indietro (velocita' negativa) dall'ultimo.
	const bool bBackward = Visual.AnimationSpeed < 0.0;
	double Time = (bBackward ? Duration : 0.0) + (Now - Visual.AnimationStart) * Visual.AnimationSpeed;
	bool bFinished = false;
	if (Visual.bAnimationLoop && Duration > 0.0)
	{
		Time = FMath::Fmod(Time, Duration);
		if (Time < 0.0) { Time += Duration; }
	}
	else if ((!bBackward && Time >= Duration) || (bBackward && Time <= 0.0))
	{
		Time = FMath::Clamp(Time, 0.0, Duration);
		bFinished = true;
	}
	GeoWorld::Models::ApplyAnimation(Rig, Visual.Animation, Time, Visual.RigState);
	ApplyRig(Visual, nullptr);
	if (bFinished) { Visual.Animation = -1; }
}

bool UGeoEntitySubsystem::SetEntityAnimation(const FString& Who, const FString& Animation, bool bLoop, double Speed, FString& OutMessage)
{
	FString Label;
	FVisual* Visual = FindVisual(Who, Label);
	if (!Visual || !Visual->FileModel.IsValid() || !Visual->FileModel->Rig.IsValid())
	{
		OutMessage = FString::Printf(TEXT("'%s': nessuna entita' a schermo con un modello da file"), *Who);
		return false;
	}
	const GeoWorld::Models::FModelRig& Rig = *Visual->FileModel->Rig;
	if (Animation.Equals(TEXT("stop"), ESearchCase::IgnoreCase))
	{
		Visual->Animation = -1;
		OutMessage = FString::Printf(TEXT("%s: animazione ferma dov'e'"), *Label);
		return true;
	}
	if (Animation.Equals(TEXT("reset"), ESearchCase::IgnoreCase))
	{
		Visual->Animation = -1;
		Visual->RigState.Poses = GeoWorld::Models::DefaultRigState(Rig).Poses;
		ApplyRig(*Visual, nullptr);
		OutMessage = FString::Printf(TEXT("%s: la posa di riposo"), *Label);
		return true;
	}
	const int32 Found = Rig.FindAnimation(std::string(TCHAR_TO_UTF8(*Animation)));
	if (Found < 0)
	{
		FString Names;
		for (const GeoWorld::Models::FRigAnimation& A : Rig.Animations) { Names += (Names.IsEmpty() ? TEXT("") : TEXT(", ")) + ToFString(A.Name); }
		OutMessage = Rig.Animations.empty()
			? FString::Printf(TEXT("%s: il modello non ha animazioni"), *Label)
			: FString::Printf(TEXT("%s: nessuna animazione '%s' (ci sono: %s)"), *Label, *Animation, *Names);
		return false;
	}
	const double Duration = Rig.Animations[static_cast<size_t>(Found)].Duration;
	Visual->Animation = Found;
	Visual->bAnimationLoop = bLoop;
	Visual->AnimationSpeed = FMath::IsNearlyZero(Speed) ? 1.0 : Speed;
	Visual->AnimationStart = FPlatformTime::Seconds();
	OutMessage = FString::Printf(TEXT("%s: animazione '%s' (%.2f s)%s, velocita' %.2f"), *Label, *ToFString(Rig.Animations[static_cast<size_t>(Found)].Name),
		Duration, bLoop ? TEXT(" in loop") : TEXT(" una volta"), Visual->AnimationSpeed);
	return true;
}
