#include "Weather/GeoLightningRenderer.h"

#include "GeoCoreModule.h"
#include "Geo/GeoUnits.h"
#include "Sky/GeoSkySubsystem.h"

#include "Components/DynamicMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/PointLightComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Engine/DirectionalLight.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "Sound/SoundWaveProcedural.h"

#include <vector>

using namespace GeoWorld;

namespace
{
	const TCHAR* LightningMaterialPath = TEXT("/GeoWorld/Materials/M_GeoLightning.M_GeoLightning");

	/** Il tuono si sintetizza a 22 kHz: il rombo e' basso, e un tuono vicino dura 10 s. */
	constexpr int32 ThunderSampleRate = 22050;
	/** Il canale piu' corto: anche con la nube bassa sul suolo (o il suolo sopra la base) si vede un fulmine. */
	constexpr double MinChannelM = 300.0;
	/** Fin dove arriva la luce (m): un fulmine illumina qualche chilometro, un lampo nella nube di piu'. */
	constexpr double BoltLightRadiusM = 15000.0;
	constexpr double CloudLightRadiusM = 25000.0;

	/**
	 * Un lampo nella nube: il suono viene da un canale quasi orizzontale in
	 * quota. Si prende un canale verticale e lo si corica nel cuore della nube.
	 */
	void LayChannelInCloud(std::vector<Weather::FBoltSegment>& Channel, double Heading, double HeightM)
	{
		const double Cosine = FMath::Cos(Heading), Sine = FMath::Sin(Heading);
		auto Lay = [Cosine, Sine, HeightM](double Point[3])
		{
			const double Up = Point[2];
			Point[0] += Up * Cosine;
			Point[1] += Up * Sine;
			Point[2] = HeightM + 0.1 * Up;
		};
		for (Weather::FBoltSegment& Segment : Channel)
		{
			Lay(Segment.A);
			Lay(Segment.B);
		}
	}
}

FGeoLightningRenderer::~FGeoLightningRenderer()
{
	Shutdown();
}

bool FGeoLightningRenderer::EnsureMaterial()
{
	// Si cerca ogni due secondi finche' manca: si puo' creare con il Play gia'
	// partito (geo.Imagery.CreateMaterial).
	const double Clock = FPlatformTime::Seconds();
	if (!Material && Clock - LastMaterialLookup > 2.0)
	{
		LastMaterialLookup = Clock;
		Material.Reset(LoadObject<UMaterialInterface>(nullptr, LightningMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet));
	}
	if (Material) { Problem.Reset(); }
	else { Problem = TEXT("manca M_GeoLightning: i fulmini illuminano ma il canale non si vede. Crealo con geo.Imagery.CreateMaterial"); }
	return Material.IsValid();
}

double FGeoLightningRenderer::ReferenceLux(UWorld* World)
{
	// Con il cielo acceso: la luce che c'e' davvero adesso (di notte quella
	// della luna), ma mai meno di un cinquantesimo del giorno: di notte il
	// fulmine acceca.
	if (const UGeoSkySubsystem* SkySubsystem = World ? World->GetSubsystem<UGeoSkySubsystem>() : nullptr)
	{
		const double SceneLux = SkySubsystem->GetSceneLux();
		if (SceneLux > 0.0) { return FMath::Max(SceneLux, 0.02 * SkySubsystem->GetSunLux()); }
	}
	// Il primo sole del livello; senza (o di notte), qualche lux: il fulmine
	// si vede comunque.
	double Lux = 0.0;
	for (TActorIterator<ADirectionalLight> It(World); It; ++It)
	{
		if (const ULightComponent* Sun = It->GetLightComponent()) { Lux = Sun->Intensity; }
		break;
	}
	return FMath::Max(Lux, 3.0);
}

double FGeoLightningRenderer::AddStrike(UWorld* World, const FGeoreferenceSnapshot& Snapshot,
                                        const Weather::FLightningStrike& Strike, double GroundHeightM,
                                        double CloudBaseM, double CloudMiddleM,
                                        const FVector& CameraLocation, double TemperatureC, double Now)
{
	if (!World) { return 0.0; }
	const bool bToGround = Strike.Kind == Weather::ELightningKind::CloudToGround;
	const Core::FGeodetic Where = Core::FGeodetic::FromDegrees(Strike.LatDeg, Strike.LonDeg, GroundHeightM);
	const FVector GroundLocation = Snapshot.GeodeticToUnreal(Where);
	// La camera vista dal punto colpito, in metri: gli assi di Unreal sono
	// (nord, est, su) all'origine, e a qualche decina di km l'errore e' di
	// qualche decimo di grado.
	const FVector3d Camera = FVector3d(CameraLocation - GroundLocation) * Units::UuToMeters;

	// Il canale (anche per un lampo nella nube: il tuono viene da li'). Nasce
	// nella base della nube di pioggia: qualche centinaio di metri dentro, dove
	// la cima del canale sfuma (BuildLightningChannel), cosi' esce dalla nuvola.
	Weather::LightningDetail::FRandom Random(Strike.Seed);
	const double ChannelHeight = FMath::Max(MinChannelM, CloudBaseM) + Random.Range(100.0, 400.0);
	const double InCloudHeight = FMath::Max(CloudMiddleM, ChannelHeight + 500.0);
	std::vector<Weather::FBoltSegment> Channel;
	Weather::BuildLightningChannel(ChannelHeight, Strike.Seed, Channel);
	if (!bToGround) { LayChannelInCloud(Channel, Random.Range(0.0, 2.0 * PI), InCloudHeight); }
	const FVector3d Flash = bToGround ? FVector3d(0.0, 0.0, 0.3 * ChannelHeight) : FVector3d(0.0, 0.0, InCloudHeight);
	const double Distance = FVector3d::Distance(Camera, bToGround ? FVector3d::ZeroVector : Flash);

	// --- Il tuono ---------------------------------------------------------------
	//  Quando arriva si sa subito (la distanza del pezzo di canale piu' vicino:
	//  la stessa che usa SynthesizeThunder); i campioni li prepara un thread di
	//  lavoro, tutto per copia, niente "this".
	const double Listener[3] = { Camera.X, Camera.Y, Camera.Z };
	double Nearest = 1.0e30;
	for (const Weather::FBoltSegment& Segment : Channel)
	{
		const FVector3d Middle(0.5 * (Segment.A[0] + Segment.B[0]), 0.5 * (Segment.A[1] + Segment.B[1]), 0.5 * (Segment.A[2] + Segment.B[2]));
		Nearest = FMath::Min(Nearest, FVector3d::Distance(Middle, Camera));
	}
	const bool bRoomForThunder = Thunders.Num() + Playing.Num() < MaxThunders || Distance < 5000.0;
	if (bRoomForThunder && Nearest <= Weather::ThunderAudibleM)
	{
		FThunderToPlay& Pending = Thunders.AddDefaulted_GetRef();
		Pending.PlayAt = Strike.TimeSeconds + Nearest / Weather::SpeedOfSound(TemperatureC);
		Pending.Samples = MakeShared<FThunderSamples, ESPMode::ThreadSafe>();
		TSharedPtr<FThunderSamples, ESPMode::ThreadSafe> Out = Pending.Samples;
		const uint32 Seed = Strike.Seed;
		Pending.Task = UE::Tasks::Launch(TEXT("GeoThunder"),
			[Out, Channel, Listener, TemperatureC, Seed]()
			{
				Weather::FThunder Thunder;
				if (Weather::SynthesizeThunder(Channel, Listener, TemperatureC, Seed, ThunderSampleRate, Thunder))
				{
					Out->SampleRate = Thunder.SampleRate;
					Out->Volume = static_cast<float>(Thunder.Loudness);
					Out->Pcm.SetNumUninitialized(static_cast<int32>(Thunder.Samples.size()));
					for (int32 Index = 0; Index < Out->Pcm.Num(); ++Index)
					{
						Out->Pcm[Index] = static_cast<int16>(FMath::Clamp(FMath::RoundToInt(Thunder.Samples[Index] * 32767.0f), -32767, 32767));
					}
				}
				Out->bReady.store(true, std::memory_order_release);
			},
			UE::Tasks::ETaskPriority::BackgroundNormal);
	}

	// --- Il fulmine a schermo ------------------------------------------------
	if (Bolts.Num() >= MaxBolts) { return Distance; }
	const bool bWithChannel = bToGround && EnsureMaterial();

	FActorSpawnParameters Parameters;
	Parameters.ObjectFlags |= RF_Transient;
	AActor* Spawned = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(GroundLocation), Parameters);
	if (!Spawned) { return Distance; }
#if WITH_EDITOR
	Spawned->SetActorLabel(TEXT("GeoWorld Lightning"));
#endif
	USceneComponent* Root = NewObject<USceneComponent>(Spawned);
	Root->SetMobility(EComponentMobility::Movable);
	Spawned->SetRootComponent(Root);
	Root->RegisterComponent();
	Spawned->SetActorLocation(GroundLocation);

	const double Lux = ReferenceLux(World);
	FBolt& Bolt = Bolts.AddDefaulted_GetRef();
	Bolt.Actor = Spawned;
	Bolt.Strike = Strike;
	Bolt.Where = Where;
	Bolt.LightHeightM = Flash.Z;
	// Al suolo, sotto la luce: una volta e mezza il sole per un nube-suolo,
	// poco piu' di mezzo per un lampo nella nube (piu' alto e diffuso).
	const double LightHeight = Flash.Z;
	Bolt.Candela = (bToGround ? 1.5 : 0.6) * Lux * LightHeight * LightHeight;
	// L'emissivo del canale si misura sulla luce della scena: con un sole
	// fisico (100.000 lux) l'esposizione e' tanto piu' bassa.
	Bolt.Emissive = FMath::Max(1.0, Lux / 10.0);

	UPointLightComponent* Light = NewObject<UPointLightComponent>(Spawned);
	Light->SetupAttachment(Root);
	Light->SetMobility(EComponentMobility::Movable);
	Light->SetCastShadows(false);
	Light->SetIntensityUnits(ELightUnits::Candelas);
	Light->SetLightColor(FLinearColor(0.78f, 0.84f, 1.0f));
	Light->SetAttenuationRadius(static_cast<float>((bToGround ? BoltLightRadiusM : CloudLightRadiusM) * Units::MetersToUu));
	Light->SetRelativeLocation(FVector(Flash * Units::MetersToUu));
	Light->SetIntensity(0.0f);
	Light->RegisterComponent();
	Bolt.Light = Light;

	if (!bWithChannel) { return Distance; }

	// ------------------------------------------------------------------
	//  I nastri: per ogni pezzo, un quadrato da A a B largo verso la camera.
	//  Il filo e' largo almeno 1,5 m, e da lontano almeno ~1 pixel (1,2 mm
	//  per metro di distanza); il nastro e' 8 volte tanto, per l'alone. UV0:
	//  x attraverso il nastro (0..1), y lungo; UV1.x: quanto e' luminoso
	//  il pezzo (1 il canale, 0,35 i rami, meno in cima dove entra nella nube).
	// ------------------------------------------------------------------
	UE::Geometry::FDynamicMesh3 Built;
	Built.EnableAttributes();
	Built.Attributes()->SetNumUVLayers(2);
	UE::Geometry::FDynamicMeshUVOverlay* Across = Built.Attributes()->GetUVLayer(0);
	UE::Geometry::FDynamicMeshUVOverlay* Weights = Built.Attributes()->GetUVLayer(1);
	UE::Geometry::FDynamicMeshNormalOverlay* Normals = Built.Attributes()->PrimaryNormals();
	for (const Weather::FBoltSegment& Segment : Channel)
	{
		if (Segment.Weight < 0.01) { continue; }
		const FVector3d A(Segment.A[0], Segment.A[1], Segment.A[2]);
		const FVector3d B(Segment.B[0], Segment.B[1], Segment.B[2]);
		const FVector3d Middle = 0.5 * (A + B);
		FVector3d Side = FVector3d::CrossProduct(B - A, Camera - Middle);
		if (!Side.Normalize()) { Side = FVector3d(1.0, 0.0, 0.0); }
		const double Core = FMath::Max(1.5, FVector3d::Distance(Camera, Middle) * 0.0012) * (Segment.Weight >= 0.99 ? 1.0 : 0.7);
		const FVector3d Half = Side * (4.0 * Core);
		const FVector3d Corners[4] = { A - Half, A + Half, B + Half, B - Half };
		static const FVector2f CornerUv[4] = { FVector2f(0.0f, 0.0f), FVector2f(1.0f, 0.0f), FVector2f(1.0f, 1.0f), FVector2f(0.0f, 1.0f) };
		int32 Vertices[4];
		int32 UvA[4], UvB[4], Up[4];
		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			Vertices[Corner] = Built.AppendVertex(Corners[Corner]);
			UvA[Corner] = Across->AppendElement(CornerUv[Corner]);
			UvB[Corner] = Weights->AppendElement(FVector2f(static_cast<float>(Segment.Weight), 0.0f));
			Up[Corner] = Normals->AppendElement(FVector3f(0.0f, 0.0f, 1.0f));
		}
		const int32 Triangles[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
		for (const int32* Triangle : Triangles)
		{
			const int32 TriangleId = Built.AppendTriangle(UE::Geometry::FIndex3i(Vertices[Triangle[0]], Vertices[Triangle[1]], Vertices[Triangle[2]]));
			if (TriangleId < 0) { continue; }
			Across->SetTriangle(TriangleId, UE::Geometry::FIndex3i(UvA[Triangle[0]], UvA[Triangle[1]], UvA[Triangle[2]]));
			Weights->SetTriangle(TriangleId, UE::Geometry::FIndex3i(UvB[Triangle[0]], UvB[Triangle[1]], UvB[Triangle[2]]));
			Normals->SetTriangle(TriangleId, UE::Geometry::FIndex3i(Up[Triangle[0]], Up[Triangle[1]], Up[Triangle[2]]));
		}
	}

	UDynamicMeshComponent* Mesh = NewObject<UDynamicMeshComponent>(Spawned);
	Mesh->SetupAttachment(Root);
	Mesh->SetMobility(EComponentMobility::Movable);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);
	Mesh->SetEnableRaytracing(false);
	// La mesh e' in METRI: la scala fa metri -> unita' Unreal (come le tile).
	Mesh->SetRelativeScale3D(FVector(Units::MetersToUu));
	Mesh->SetMaterial(0, Material.Get());
	Mesh->SetMesh(MoveTemp(Built));
	Mesh->RegisterComponent();
	Mesh->NotifyMeshUpdated();
	Mesh->SetCustomPrimitiveDataFloat(0, 0.0f);
	Bolt.Mesh = Mesh;
	return Distance;
}

void FGeoLightningRenderer::Update(UWorld* World, const FGeoreferenceSnapshot& Snapshot, double Now)
{
	if (!World) { return; }

	// --- I fulmini: la luce di adesso, e al loro posto dopo un rebase ---------
	for (int32 Index = Bolts.Num() - 1; Index >= 0; --Index)
	{
		FBolt& Bolt = Bolts[Index];
		AActor* Owner = Bolt.Actor.Get();
		const double Since = Now - Bolt.Strike.TimeSeconds;
		if (!Owner || Since > Weather::FlashDuration(Bolt.Strike) + 0.1)
		{
			if (Owner) { Owner->Destroy(); }
			Bolts.RemoveAtSwap(Index);
			continue;
		}
		const double Light = Weather::FlashBrightness(Bolt.Strike, Since);
		Owner->SetActorLocation(Snapshot.GeodeticToUnreal(Bolt.Where));
		if (UPointLightComponent* Lamp = Bolt.Light.Get()) { Lamp->SetIntensity(static_cast<float>(Bolt.Candela * Light)); }
		if (UDynamicMeshComponent* Mesh = Bolt.Mesh.Get())
		{
			Mesh->SetCustomPrimitiveDataFloat(0, static_cast<float>(Bolt.Emissive * Light));
			Mesh->SetVisibility(Light > 0.002);
		}
	}

	// --- I tuoni che arrivano adesso (se il thread di lavoro li ha pronti) ------
	for (int32 Index = Thunders.Num() - 1; Index >= 0; --Index)
	{
		const FThunderToPlay& Thunder = Thunders[Index];
		if (Thunder.PlayAt > Now) { continue; }
		const bool bReady = Thunder.Samples && Thunder.Samples->bReady.load(std::memory_order_acquire);
		// Pronto: suona. Non ancora pronto da due secondi: qualcosa non va, si lascia perdere.
		if (bReady) { PlayThunder(World, *Thunder.Samples, Now); }
		if (bReady || Now - Thunder.PlayAt > 2.0) { Thunders.RemoveAtSwap(Index); }
	}
	Playing.RemoveAll([Now](const FPlayingThunder& Sound) { return Sound.Until < Now; });
}

void FGeoLightningRenderer::PlayThunder(UWorld* World, const FThunderSamples& Thunder, double Now)
{
	if (Thunder.Pcm.Num() == 0 || ThunderVolume <= 0.0) { return; }

	// ------------------------------------------------------------------
	//  NOTA UE: USoundWaveProcedural e' un suono i cui campioni li da' il
	//  codice invece di un file. Qui li si mette tutti in coda subito
	//  (QueueAudio, PCM a 16 bit) e lo si suona come un suono qualunque.
	//  2D: il tuono arriva da tutto il cielo.
	// ------------------------------------------------------------------
	USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>();
	Wave->SetSampleRate(Thunder.SampleRate);
	Wave->NumChannels = 1;
	Wave->Duration = static_cast<float>(Thunder.Pcm.Num()) / static_cast<float>(Thunder.SampleRate);
	Wave->SoundGroup = SOUNDGROUP_Default;
	Wave->bLooping = false;
	Wave->QueueAudio(reinterpret_cast<const uint8*>(Thunder.Pcm.GetData()), Thunder.Pcm.Num() * static_cast<int32>(sizeof(int16)));
	UGameplayStatics::PlaySound2D(World, Wave, static_cast<float>(Thunder.Volume * ThunderVolume));

	// Il suono non e' una UPROPERTY di nessuno: lo si tiene in vita finche' suona.
	FPlayingThunder& Sound = Playing.AddDefaulted_GetRef();
	Sound.Wave.Reset(Wave);
	Sound.Until = Now + Wave->Duration + 1.0;
}

bool FGeoLightningRenderer::GetBrightestFlash(const FGeoreferenceSnapshot& Snapshot, double Now,
                                              FVector& OutLocation, double& OutBrightness) const
{
	const FBolt* Brightest = nullptr;
	double Best = 0.002;
	for (const FBolt& Bolt : Bolts)
	{
		const double Light = Weather::FlashBrightness(Bolt.Strike, Now - Bolt.Strike.TimeSeconds);
		if (Light > Best) { Best = Light; Brightest = &Bolt; }
	}
	if (!Brightest) { return false; }
	Core::FGeodetic Lamp = Brightest->Where;
	Lamp.HeightM += Brightest->LightHeightM;
	OutLocation = Snapshot.GeodeticToUnreal(Lamp);
	OutBrightness = Best;
	return true;
}

double FGeoLightningRenderer::NextThunderIn(double Now) const
{
	double Next = -1.0;
	for (const FThunderToPlay& Thunder : Thunders)
	{
		const double In = Thunder.PlayAt - Now;
		if (In >= 0.0 && (Next < 0.0 || In < Next)) { Next = In; }
	}
	return Next;
}

void FGeoLightningRenderer::Shutdown()
{
	for (FBolt& Bolt : Bolts)
	{
		if (AActor* Owner = Bolt.Actor.Get()) { Owner->Destroy(); }
	}
	Bolts.Reset();
	// I lavori in volo eseguono codice di questo modulo: si aspettano.
	for (FThunderToPlay& Thunder : Thunders) { Thunder.Task.Wait(); }
	Thunders.Reset();
	Playing.Reset();
}
