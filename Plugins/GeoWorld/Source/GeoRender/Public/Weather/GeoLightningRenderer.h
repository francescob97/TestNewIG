// =============================================================================
//  GeoLightningRenderer.h -- I fulmini a schermo e il tuono. STRATO: UNREAL.
//
//  Ogni fulmine e' un attore transient che vive meno di un secondo:
//
//   - IL CANALE: una mesh di nastri, uno per pezzo di canale
//     (Weather/Lightning.h, BuildLightningChannel), dalla base della nube di
//     pioggia (Weather/Clouds.h) al suolo, girati verso la camera
//     quando il fulmine nasce (in mezzo secondo la camera non si sposta
//     abbastanza da accorgersene). Materiale M_GeoLightning: additivo,
//     emissivo, un filo accecante dentro un alone. La luce di ogni istante
//     arriva nei Custom Primitive Data: ogni fulmine sfarfalla per conto suo.
//   - LA LUCE: una point light, perche' un fulmine illumina il paesaggio. Al
//     suolo vale una volta e mezza il sole del livello (di notte, comunque
//     qualche lux): si vede di giorno e acceca di notte, come quello vero.
//     Un lampo nella nube e' solo la luce, in alto e larga.
//   - IL TUONO: sintetizzato (Weather/Lightning.h, SynthesizeThunder) su un
//     thread di lavoro -- per un fulmine vicino sono 8-16 ms, un frame intero
//     sul game thread -- e suonato quando arriva: dopo distanza / velocita'
//     del suono. Sul game thread si calcola solo QUANDO arriva.
//
//  Il fulmine si ricorda DOVE e' caduto in coordinate geografiche e si
//  rimette a posto a ogni frame: un rebase nel mezzo non lo sposta.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Tasks/Task.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include <atomic>

#include "Georeference/GeoreferenceSnapshot.h"
#include "Weather/Lightning.h"

class AActor;
class UDynamicMeshComponent;
class UMaterialInterface;
class UPointLightComponent;
class USoundWaveProcedural;
class UWorld;

class GEORENDER_API FGeoLightningRenderer
{
public:
	/** Al massimo tanti fulmini a schermo, e tanti tuoni in coda o che suonano. */
	static constexpr int32 MaxBolts = 12;
	static constexpr int32 MaxThunders = 8;

	~FGeoLightningRenderer();

	/**
	 * Un fulmine nuovo. GroundHeightM: la quota ellissoidica del suolo nel
	 * punto (colpito, o sotto il lampo nella nube); CloudBaseM e CloudMiddleM:
	 * la base della nube di pioggia e il suo cuore, m SOPRA IL SUOLO li' (il
	 * canale nasce nella base, il lampo nella nube sta nel cuore);
	 * CameraLocation in unita' Unreal; Now l'orologio dello scheduler.
	 * Ritorna la distanza dalla camera, m.
	 */
	double AddStrike(UWorld* World, const FGeoreferenceSnapshot& Snapshot, const GeoWorld::Weather::FLightningStrike& Strike,
	                 double GroundHeightM, double CloudBaseM, double CloudMiddleM,
	                 const FVector& CameraLocation, double TemperatureC, double Now);

	/**
	 * Il lampo piu' forte adesso, per accendere le nuvole attorno: dove
	 * (unita' Unreal: il punto della luce) e quanto (FlashBrightness, 0..1).
	 * False se non ce n'e' nessuno acceso.
	 */
	bool GetBrightestFlash(const FGeoreferenceSnapshot& Snapshot, double Now, FVector& OutLocation, double& OutBrightness) const;

	/** Una volta per frame: luce dei fulmini, posizione (rebase), tuoni che arrivano, pulizia. */
	void Update(UWorld* World, const FGeoreferenceSnapshot& Snapshot, double Now);

	/** Via tutto, subito. */
	void Shutdown();

	void SetThunderVolume(double Volume) { ThunderVolume = FMath::Clamp(Volume, 0.0, 2.0); }
	double GetThunderVolume() const { return ThunderVolume; }
	/** Vuoto se tutto c'e'; altrimenti cosa manca. */
	const FString& GetProblem() const { return Problem; }
	int32 GetActiveBolts() const { return Bolts.Num(); }
	/** Fra quanto arriva il prossimo tuono (s); negativo se non ce ne sono in arrivo. */
	double NextThunderIn(double Now) const;
	/** Quanta luce c'e' nel livello (il sole, lux): il fulmine si misura su quella (anche nelle nuvole). */
	static double ReferenceLux(UWorld* World);

private:
	struct FBolt
	{
		TWeakObjectPtr<AActor> Actor;
		TWeakObjectPtr<UDynamicMeshComponent> Mesh;
		TWeakObjectPtr<UPointLightComponent> Light;
		GeoWorld::Weather::FLightningStrike Strike;
		GeoWorld::Core::FGeodetic Where;
		/** Quanto sopra il suolo sta la luce, m. */
		double LightHeightM = 0.0;
		/** La luce al primo colpo (cd), e quanto si moltiplica l'emissivo del canale. */
		double Candela = 0.0;
		double Emissive = 1.0;
	};

	/** I campioni di un tuono, scritti dal thread di lavoro: si leggono solo con bReady. */
	struct FThunderSamples
	{
		std::atomic<bool> bReady{ false };
		TArray<int16> Pcm;
		int32 SampleRate = 22050;
		float Volume = 1.0f;
	};

	struct FThunderToPlay
	{
		/** Quando arriva, sull'orologio dello scheduler: calcolato subito, sul game thread. */
		double PlayAt = 0.0;
		TSharedPtr<FThunderSamples, ESPMode::ThreadSafe> Samples;
		/** Il lavoro che li prepara (non dentro Samples: il lavoro tiene Samples, sarebbe un cerchio). */
		UE::Tasks::FTask Task;
	};

	struct FPlayingThunder
	{
		TStrongObjectPtr<USoundWaveProcedural> Wave;
		double Until = 0.0;
	};

	bool EnsureMaterial();
	void PlayThunder(UWorld* World, const FThunderSamples& Samples, double Now);

	TStrongObjectPtr<UMaterialInterface> Material;
	double LastMaterialLookup = -1.0e9;

	TArray<FBolt> Bolts;
	TArray<FThunderToPlay> Thunders;
	TArray<FPlayingThunder> Playing;
	double ThunderVolume = 1.0;
	FString Problem;
};
