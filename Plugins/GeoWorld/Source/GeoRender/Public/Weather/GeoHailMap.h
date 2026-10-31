// =============================================================================
//  GeoHailMap.h -- La grandine al suolo, in Unreal. STRATO: UNREAL.
//
//  Tiene la griglia della grandine posata (Weather/GroundHail.h) e la fa
//  arrivare ai materiali del terreno e delle strade come TEXTURE:
//
//   - ogni 4 secondi un thread di lavoro avanza la griglia (la grandine che
//     cade, quella che fonde) e la ricentra se la camera si e' allontanata
//     di 15 km. Sono 65.536 celle, ognuna con il rumore della mappa dei
//     rovesci: un centinaio di millisecondi, troppi per il game thread;
//   - quando il lavoro e' pronto, la texture si aggiorna SUL POSTO
//     (UpdateTextureRegions): l'oggetto e' sempre lo stesso, quindi le
//     istanze di materiale delle tile lo ricevono una volta sola, quando
//     nascono, e non a ogni aggiornamento;
//   - senza grandine (ne' che cade ne' posata) non lavora nessuno.
//
//  Dove sta la griglia nel mondo (il centro negli assi di Unreal, che
//  cambia con il rebase, il lato, la correzione dell'est) lo dice
//  GetPlacement, per la collezione dei parametri (GroundHailMap).
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Tasks/Task.h"
#include "UObject/StrongObjectPtr.h"

#include <atomic>
#include <vector>

#include "Georeference/GeoreferenceSnapshot.h"
#include "Weather/GroundHail.h"

class UTexture2D;

class GEORENDER_API FGeoHailMap
{
public:
	/** Ogni quanto si avanza la griglia (s), e di quanto la camera puo' allontanarsi dal centro (m). */
	static constexpr double RefreshSeconds = 4.0;
	static constexpr double RecentreDistanceM = 15000.0;
	/** Un passo lungo (un frame bloccato, il debugger) non deve far cadere un'ora di grandine. */
	static constexpr double MaxStepSeconds = 60.0;

	~FGeoHailMap();

	/**
	 * Una volta per frame. Il tempo e' quello della precipitazione (GetTime):
	 * fermo lui, ferma la grandine. GroundTemperatureC: l'aria al suolo sotto
	 * la camera (la griglia non conosce le quote).
	 */
	void Update(const GeoWorld::Weather::FPrecipitationField& Field, bool bHaveCamera,
	            double CameraLatDeg, double CameraLonDeg, double GroundTemperatureC);

	/** Grandine a mano: un disco attorno al punto (per provare). */
	void AddDisc(double LatDeg, double LonDeg, double RadiusM, double DepthMm);
	/** Via tutta la grandine posata. */
	void Clear();
	/** Via tutto, subito: aspetta il lavoro in volo. */
	void Shutdown();

	/** La texture per i materiali (nulla finche' non serve la prima volta). */
	UTexture2D* GetTexture() const { return Texture.Get(); }

	/**
	 * (centro nord, centro est in metri negli assi di Unreal, lato in metri,
	 * correzione dell'est): il parametro GroundHailMap. Senza griglia, un lato
	 * di un metro all'origine: nessuna grandine.
	 */
	FLinearColor GetPlacement(const FGeoreferenceSnapshot& Snapshot) const;

	double CoverAt(double LatDeg, double LonDeg) const { return Grid.CoverAt(LatDeg, LonDeg); }
	double MaxDepth() const { return Grid.MaxDepth(); }
	bool IsEmpty() const { return Grid.IsEmpty(); }
	bool IsWorking() const { return Job.IsValid(); }

private:
	/** Il lavoro: una copia della griglia, avanzata sul thread; si legge solo con bReady. */
	struct FJob
	{
		std::atomic<bool> bReady{ false };
		GeoWorld::Weather::FHailGround Grid;
		std::vector<uint8_t> Bgra;
	};

	/** Le modifiche a mano fatte mentre un lavoro era in volo: si rifanno sul suo risultato. */
	struct FPendingDisc
	{
		double LatDeg = 0.0, LonDeg = 0.0, RadiusM = 0.0, DepthMm = 0.0;
	};

	void FinishJob();
	/** La griglia sulla texture: la prima volta la crea, poi la aggiorna sul posto. */
	void Upload(const std::vector<uint8_t>& Bgra);
	void UploadGrid();

	GeoWorld::Weather::FHailGround Grid;
	TSharedPtr<FJob, ESPMode::ThreadSafe> Job;
	UE::Tasks::FTask JobTask;
	TArray<FPendingDisc> PendingDiscs;
	bool bPendingClear = false;

	/** L'ultimo passo, sull'orologio della precipitazione. */
	double LastStepClock = -1.0;
	TStrongObjectPtr<UTexture2D> Texture;
	/** La texture e' tutta vuota: non serve ricaricarla vuota. */
	bool bTextureEmpty = true;
};
