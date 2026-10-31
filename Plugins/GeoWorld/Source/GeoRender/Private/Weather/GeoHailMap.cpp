#include "Weather/GeoHailMap.h"

#include "Geo/GeoUnits.h"
#include "Imagery/GeoRuntimeTexture.h"

#include "Engine/Texture2D.h"
#include "RHI.h"

using namespace GeoWorld;

FGeoHailMap::~FGeoHailMap()
{
	Shutdown();
}

void FGeoHailMap::FinishJob()
{
	if (!Job || !Job->bReady.load(std::memory_order_acquire)) { return; }
	Grid = MoveTemp(Job->Grid);
	std::vector<uint8_t> Bgra = MoveTemp(Job->Bgra);
	Job.Reset();
	JobTask = UE::Tasks::FTask();

	// Cio' che si e' fatto a mano nel frattempo vale anche sul risultato.
	if (bPendingClear || PendingDiscs.Num() > 0)
	{
		if (bPendingClear) { Grid.Clear(); }
		for (const FPendingDisc& Disc : PendingDiscs) { Grid.AddDisc(Disc.LatDeg, Disc.LonDeg, Disc.RadiusM, Disc.DepthMm); }
		bPendingClear = false;
		PendingDiscs.Reset();
		UploadGrid();
		return;
	}
	// Il lavoro ha gia' scritto i pixel: sul game thread resta solo la copia.
	if (!Grid.IsEmpty() || !bTextureEmpty) { Upload(Bgra); }
}

void FGeoHailMap::UploadGrid()
{
	std::vector<uint8_t> Bgra;
	Grid.WriteBgra(Bgra);
	Upload(Bgra);
}

void FGeoHailMap::Upload(const std::vector<uint8_t>& Bgra)
{
	const int32 Size = Grid.GetSize();
	if (Bgra.size() != static_cast<size_t>(Size) * Size * 4) { return; }

	// La prima volta si crea la texture (dati, non colori: lineare, bordi
	// bloccati); poi si aggiorna SEMPRE LA STESSA, cosi' le istanze di
	// materiale delle tile la ricevono una volta sola.
	if (!Texture)
	{
		Texture.Reset(CreateGeoRuntimeDataTexture(Size, Size, Bgra, {}, /*bSrgb=*/false, false, false));
		bTextureEmpty = Grid.IsEmpty();
		return;
	}

	// ------------------------------------------------------------------
	//  NOTA UE: UpdateTextureRegions non copia subito. Il render thread fa
	//  la copia sulla scheda video piu' tardi, quindi i pixel e la regione
	//  devono vivere fino ad allora: si copiano in memoria nostra, e la
	//  funzione di pulizia (chiamata dal render thread a copia fatta) la
	//  libera. Passare Bgra.data() direttamente leggerebbe memoria gia'
	//  liberata.
	// ------------------------------------------------------------------
	uint8* Pixels = new uint8[Bgra.size()];
	FMemory::Memcpy(Pixels, Bgra.data(), Bgra.size());
	FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, static_cast<uint32>(Size), static_cast<uint32>(Size));
	Texture->UpdateTextureRegions(0, 1, Region, static_cast<uint32>(Size) * 4u, 4u, Pixels,
		[](uint8* Data, const FUpdateTextureRegion2D* Regions)
		{
			delete[] Data;
			delete Regions;
		});
	bTextureEmpty = Grid.IsEmpty();
}

void FGeoHailMap::Update(const Weather::FPrecipitationField& Field, bool bHaveCamera,
                         double CameraLatDeg, double CameraLonDeg, double GroundTemperatureC)
{
	FinishJob();
	if (!Texture) { UploadGrid(); }   // vuota: le istanze di materiale l'hanno da subito
	if (Job) { return; }

	const double Clock = Field.GetTime();
	if (LastStepClock < 0.0 || Clock < LastStepClock) { LastStepClock = Clock; }
	if (!Grid.IsCentred())
	{
		if (!bHaveCamera) { return; }
		Grid.Recentre(CameraLatDeg, CameraLonDeg);
	}
	const double Step = Clock - LastStepClock;
	if (Step < RefreshSeconds) { return; }
	LastStepClock = Clock;

	bool bRecentre = false;
	if (bHaveCamera)
	{
		const double North = (CameraLatDeg - Grid.GetCentreLatDeg()) * 111132.0;
		const double East = (CameraLonDeg - Grid.GetCentreLonDeg()) * 111320.0 * FMath::Cos(FMath::DegreesToRadians(CameraLatDeg));
		bRecentre = North * North + East * East > RecentreDistanceM * RecentreDistanceM;
	}

	const Weather::FPrecipitationSettings& Settings = Field.GetSettings();
	const bool bFalling = Settings.Kind == Weather::EPrecipitationKind::Hail && Settings.RateMmH > 0.0 && Settings.Coverage > 0.0;
	if (!bFalling && Grid.IsEmpty())
	{
		// Niente da calcolare: si segue solo la camera (una griglia vuota si
		// sposta senza perdere niente).
		if (bRecentre) { Grid.Recentre(CameraLatDeg, CameraLonDeg); }
		return;
	}

	// Il lavoro: tutto per copia, niente "this" (Shutdown lo aspetta comunque).
	Job = MakeShared<FJob, ESPMode::ThreadSafe>();
	Job->Grid = Grid;
	TSharedPtr<FJob, ESPMode::ThreadSafe> Out = Job;
	const Weather::FPrecipitationField Falling = Field;
	const double Seconds = FMath::Min(Step, MaxStepSeconds);
	JobTask = UE::Tasks::Launch(TEXT("GeoHailMap"),
		[Out, Falling, Seconds, GroundTemperatureC, bRecentre, CameraLatDeg, CameraLonDeg]()
		{
			if (bRecentre) { Out->Grid.Recentre(CameraLatDeg, CameraLonDeg); }
			Out->Grid.Advance(Falling, Seconds, GroundTemperatureC);
			Out->Grid.WriteBgra(Out->Bgra);
			Out->bReady.store(true, std::memory_order_release);
		},
		UE::Tasks::ETaskPriority::BackgroundNormal);
}

void FGeoHailMap::AddDisc(double LatDeg, double LonDeg, double RadiusM, double DepthMm)
{
	if (Job)
	{
		PendingDiscs.Add(FPendingDisc{ LatDeg, LonDeg, RadiusM, DepthMm });
		return;
	}
	if (!Grid.IsCentred()) { Grid.Recentre(LatDeg, LonDeg); }
	Grid.AddDisc(LatDeg, LonDeg, RadiusM, DepthMm);
	UploadGrid();
}

void FGeoHailMap::Clear()
{
	if (Job)
	{
		bPendingClear = true;
		PendingDiscs.Reset();
		return;
	}
	Grid.Clear();
	if (!bTextureEmpty) { UploadGrid(); }
}

FLinearColor FGeoHailMap::GetPlacement(const FGeoreferenceSnapshot& Snapshot) const
{
	if (!Grid.IsCentred()) { return FLinearColor(0.0f, 0.0f, 1.0f, 1.0f); }
	const FVector Centre = Snapshot.GeodeticToUnreal(
		Core::FGeodetic::FromDegrees(Grid.GetCentreLatDeg(), Grid.GetCentreLonDeg(), 0.0));
	return FLinearColor(static_cast<float>(Centre.X * Units::UuToMeters), static_cast<float>(Centre.Y * Units::UuToMeters),
		static_cast<float>(Grid.GetExtentM()), static_cast<float>(Grid.GetEastScale()));
}

void FGeoHailMap::Shutdown()
{
	// Il lavoro in volo esegue codice di questo modulo: si aspetta.
	if (Job) { JobTask.Wait(); }
	Job.Reset();
	JobTask = UE::Tasks::FTask();
	PendingDiscs.Reset();
	bPendingClear = false;
}
