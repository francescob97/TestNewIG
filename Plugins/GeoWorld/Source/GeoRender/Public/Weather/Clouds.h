// =============================================================================
//  Clouds.h -- Le nuvole. STRATO: C++ PURO, header-only.
//
//  Due tipi di nuvole, due origini:
//
//   1. LE NUBI DI PIOGGIA vengono dalla stessa mappa dei rovesci
//      (Precipitation.h, MapNoise): dove piove c'e' sempre nuvola, e la
//      nuvola si allarga un po' oltre la pioggia. Una pioggia estesa (un
//      fronte) e' un cielo coperto, alto fino a 6 km; i rovesci sono torri, e
//      nei nuclei (i temporali) le cime arrivano a 10 km. La base sta piu' in
//      basso dei cumuli: il cielo di pioggia e' basso.
//   2. I CUMULI DI BEL TEMPO: chiazze di un chilometro e mezzo con la base
//      piatta (dove l'aria che sale arriva a condensare) e la cima a cupola.
//      Quanti, lo dice la copertura; li disegna lo shader.
//
//  COME ARRIVANO ALLO SHADER. Il rumore della pioggia usa hash a 64 bit, troppo
//  costosi da rifare in uno shader che lo chiamerebbe decine di volte per
//  pixel. Si fa come i sistemi di nuvole dei giochi: una MAPPA METEO, una
//  texture di 256 x 256 attorno alla camera (160 km), calcolata dal C++ su un
//  thread di lavoro ogni qualche secondo: copertura, cima, pioggia. Fra un
//  ricalcolo e l'altro lo shader la sposta di quanto ha soffiato il vento in
//  quota (FCloudShaderParameters::Shift): le nuvole viaggiano lisce.
//
//  Lo shader (Weather/CloudShader.inl) e' scritto in una sintassi che e' HLSL
//  e C++ insieme: la factory dei materiali lo incolla nel materiale, i test
//  lo compilano come C++. Quello che si prova e' quello che gira.
// =============================================================================
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "Weather/Precipitation.h"

namespace GeoWorld::Weather
{
	struct FCloudSettings
	{
		/** I cumuli di bel tempo: quanta parte del cielo coprono, 0..1. */
		double Cover = 0.25;
		/** La base dei cumuli, m s.l.m. Le nubi di pioggia stanno piu' in basso (RainBaseFraction). */
		double BaseM = 1500.0;
		/** Quanto sono alti i cumuli piu' grossi, m. */
		double FairThicknessM = 1200.0;
		/** Quanto e' fitta una nuvola piena: estinzione per metro (0,03: dentro si vede a ~130 m). */
		double ExtinctionPerM = 0.03;
	};

	/** La base delle nubi di pioggia, rispetto a quella dei cumuli. */
	inline constexpr double RainBaseFraction = 0.65;

	/** Una colonna di cielo, per la mappa: tutto 0..1. */
	struct FCloudColumn
	{
		/** Quanta nube di pioggia c'e' (1 dove piove). */
		double Cover = 0.0;
		/** Fin dove arriva, fra la base e la cima delle nubi di pioggia. */
		double TopFraction = 0.0;
		/** Quanto piove (il livello della precipitazione). */
		double Rain = 0.0;
	};

	/** La base delle nubi di pioggia, m s.l.m. */
	inline double RainCloudBaseM(const FCloudSettings& Settings)
	{
		return std::max(300.0, RainBaseFraction * Settings.BaseM);
	}

	/** Fin dove arriva la nube di pioggia di una colonna (TopFraction), m s.l.m.: come RainTop nello shader. */
	inline double RainCloudTopM(const FCloudSettings& Settings, const FPrecipitationField& Field, double TopFraction)
	{
		const double Base = RainCloudBaseM(Settings);
		return Base + 600.0 + TopFraction * (Field.CloudTopM() - Base - 600.0);
	}

	/**
	 * La nube di pioggia sopra un punto. Dove piove la copertura e' piena; la
	 * nuvola si allarga attorno alla pioggia (il rumore della mappa fino a 0,9
	 * deviazioni sotto la soglia della pioggia), e le cime sono alte nei
	 * nuclei e basse ai margini. Una pioggia estesa e' tutto coperto.
	 */
	inline FCloudColumn CloudColumn(const FPrecipitationField& Field, double LatDeg, double LonDeg)
	{
		using PrecipitationDetail::Smoothstep;
		FCloudColumn Column;
		const FPrecipitationSettings& Settings = Field.GetSettings();
		if (Settings.RateMmH <= 0.0 || Settings.Coverage <= 0.0) { return Column; }
		const double Noise = Field.MapNoise(LatDeg, LonDeg);
		Column.Rain = Field.LevelFromNoise(Noise);
		if (Settings.Coverage >= 0.999)
		{
			Column.Cover = 1.0;
			Column.TopFraction = 0.55 + 0.45 * Smoothstep(0.5, 1.0, Column.Rain);
			return Column;
		}
		const double Threshold = Field.NoiseThreshold();
		Column.Cover = Smoothstep(Threshold - 0.9, Threshold - 0.1, Noise);
		Column.TopFraction = Column.Rain > 0.0 ? 0.35 + 0.65 * Smoothstep(0.15, 1.0, Column.Rain) : 0.25 * Column.Cover;
		return Column;
	}

	// =========================================================================
	//  La mappa meteo per lo shader
	// =========================================================================

	struct FCloudMap
	{
		/** Lato in texel, e quanti metri copre (lato), centrata in (CenterLat, CenterLon). */
		int32_t Size = 0;
		double ExtentM = 0.0;
		double CenterLatDeg = 0.0, CenterLonDeg = 0.0;
		/** Lo spostamento della mappa della pioggia quando la si e' fatta: lo shader aggiunge quello successivo. */
		double DriftNorthM = 0.0, DriftEastM = 0.0;
		/** BGRA, riga per riga da sud a nord, colonna da ovest a est: R = copertura, G = cima, B = pioggia. */
		std::vector<uint8_t> Bgra;
	};

	/** Dove sta il centro del texel (Row, Column), m dal centro della mappa (nord, est). */
	inline void CloudMapTexelOffset(const FCloudMap& Map, int32_t Row, int32_t Column, double& OutNorthM, double& OutEastM)
	{
		OutNorthM = ((Row + 0.5) / Map.Size - 0.5) * Map.ExtentM;
		OutEastM = ((Column + 0.5) / Map.Size - 0.5) * Map.ExtentM;
	}

	inline void BuildCloudMap(const FPrecipitationField& Field, double CenterLatDeg, double CenterLonDeg,
	                          double ExtentM, int32_t Size, FCloudMap& Out)
	{
		Out.Size = Size;
		Out.ExtentM = ExtentM;
		Out.CenterLatDeg = CenterLatDeg;
		Out.CenterLonDeg = CenterLonDeg;
		Out.DriftNorthM = Field.GetDriftNorth();
		Out.DriftEastM = Field.GetDriftEast();
		Out.Bgra.assign(static_cast<size_t>(Size) * Size * 4, 0);
		const double MetresEast = 111320.0 * std::cos(CenterLatDeg * WindDegToRad);
		auto ToByte = [](double Value) { return static_cast<uint8_t>(std::lround(PrecipitationDetail::Clamp(Value, 0.0, 1.0) * 255.0)); };
		for (int32_t Row = 0; Row < Size; ++Row)
		{
			for (int32_t Column = 0; Column < Size; ++Column)
			{
				double North = 0.0, East = 0.0;
				CloudMapTexelOffset(Out, Row, Column, North, East);
				const FCloudColumn Sky = CloudColumn(Field, CenterLatDeg + North / 111132.0, CenterLonDeg + East / MetresEast);
				uint8_t* Pixel = &Out.Bgra[(static_cast<size_t>(Row) * Size + Column) * 4];
				Pixel[0] = ToByte(Sky.Rain);
				Pixel[1] = ToByte(Sky.TopFraction);
				Pixel[2] = ToByte(Sky.Cover);
				Pixel[3] = 255;
			}
		}
	}

	// =========================================================================
	//  I parametri dello shader (Weather/CloudShader.inl)
	// =========================================================================

	/**
	 * Sette vettori, gli stessi nomi della collezione MPC_GeoWeather. Tutte le
	 * posizioni sono in metri negli assi di Unreal all'origine (nord, est, su).
	 */
	struct FCloudShaderParameters
	{
		/** Il centro della mappa (nord, est), il suo lato. */
		float Map[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
		/** Di quanto il vento in quota ha spostato le nuvole da quando la mappa e' stata fatta (nord, est). */
		float Shift[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		/** La base dei cumuli, la cima delle nubi di pioggia, la soglia dei cumuli (FairCloudThreshold), il loro spessore (m). */
		float Layer[4] = { 1500.0f, 6000.0f, 0.0f, 1200.0f };
		/** L'estinzione, il geoide all'origine (per la quota sul mare), la base delle nubi di pioggia, il tempo (s, dentro CloudTimePeriodS). */
		float Look[4] = { 0.03f, 0.0f, 1000.0f, 0.0f };
		/** Il lampo dentro le nuvole: dove (nord, est, su) e quanto. */
		float Flash[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		/** Dove sta l'origine nel disegno dei cumuli (m, nord ed est, gia' spostati dal vento, dentro FairPatternPeriodM). */
		float Fair[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	};

	/**
	 * Il disegno dei cumuli si ripete ogni 1500 km, e il loro cambiare ogni
	 * giorno: il rumore dello shader e' periodico con gli stessi passi
	 * (CloudShader.inl), quindi riportare l'origine del disegno o l'orologio
	 * dentro il periodo -- i numeri restano piccoli per un float -- non si vede.
	 * Cambiarli vuol dire cambiare anche i periodi scritti nello shader.
	 */
	inline constexpr double FairPatternPeriodM = 1.5e6;
	inline constexpr double CloudTimePeriodS = 86400.0;

	/**
	 * La soglia del disegno dei cumuli (Layer.z dello shader) che da' la
	 * copertura chiesta: la parte delle colonne di cielo con dentro un po' di
	 * nuvola, a qualunque quota (quello che si vede guardando in su). MISURATA
	 * sullo shader stesso, compilato come C++, su 40.000 colonne: il rumore non
	 * e' uniforme e i bordi sfrangiati mangiano un po' di nuvola, quindi niente
	 * formula, una tabella. Copertura zero: niente cumuli.
	 */
	inline double FairCloudThreshold(double Cover)
	{
		static const double Thresholds[] = { 0.00, 0.05, 0.10, 0.15, 0.20, 0.25, 0.30, 0.35, 0.40,
		                                     0.45, 0.50, 0.55, 0.60, 0.65, 0.70, 0.75, 0.80, 0.85 };
		static const double Covers[] = { 1.000, 0.999, 0.994, 0.979, 0.950, 0.905, 0.839, 0.759, 0.657,
		                                 0.545, 0.426, 0.314, 0.219, 0.135, 0.073, 0.033, 0.012, 0.003 };
		constexpr int32_t Count = static_cast<int32_t>(sizeof(Thresholds) / sizeof(Thresholds[0]));
		if (!(Cover > 0.0)) { return 2.0; }
		if (Cover >= Covers[0]) { return Thresholds[0]; }
		for (int32_t Index = 0; Index + 1 < Count; ++Index)
		{
			if (Cover >= Covers[Index + 1])
			{
				const double T = (Cover - Covers[Index + 1]) / (Covers[Index] - Covers[Index + 1]);
				return Thresholds[Index + 1] + T * (Thresholds[Index] - Thresholds[Index + 1]);
			}
		}
		return Thresholds[Count - 1];
	}

	/**
	 * I parametri di adesso. MapCenterNorthM/EastM: dove sta il centro della
	 * mappa negli assi di Unreal, in metri (cambia con il rebase);
	 * OriginLat/Lon: l'origine del mondo; ReferenceLat/Lon: un punto FISSO
	 * (il primo posto visto), a cui si ancora il disegno dei cumuli;
	 * GeoidAtOriginM: per la quota sul mare.
	 *
	 * PERCHE' UN RIFERIMENTO FISSO. Il disegno dei cumuli sta in metri verso
	 * nord e verso est. Con "est = longitudine x 111.320 x cos(latitudine
	 * dell'origine)" un rebase di 10 km verso nord cambiava il coseno, e i
	 * cumuli saltavano di quasi un chilometro verso est. Con il coseno di un
	 * punto fisso l'est e' lineare nella longitudine, e un rebase sposta il
	 * disegno di quanto si e' spostata l'origine: niente salti.
	 */
	inline FCloudShaderParameters MakeCloudShaderParameters(const FCloudSettings& Settings, const FPrecipitationField& Field,
	                                                        const FCloudMap& Map, double MapCenterNorthM, double MapCenterEastM,
	                                                        double OriginLatDeg, double OriginLonDeg,
	                                                        double ReferenceLatDeg, double ReferenceLonDeg,
	                                                        double GeoidAtOriginM, double TimeSeconds)
	{
		FCloudShaderParameters Out;
		Out.Map[0] = static_cast<float>(MapCenterNorthM);
		Out.Map[1] = static_cast<float>(MapCenterEastM);
		Out.Map[2] = static_cast<float>(Map.ExtentM > 0.0 ? Map.ExtentM : 1.0);
		Out.Shift[0] = static_cast<float>(Field.GetDriftNorth() - Map.DriftNorthM);
		Out.Shift[1] = static_cast<float>(Field.GetDriftEast() - Map.DriftEastM);
		Out.Layer[0] = static_cast<float>(Settings.BaseM);
		Out.Layer[1] = static_cast<float>(Field.CloudTopM());
		Out.Layer[2] = static_cast<float>(FairCloudThreshold(Settings.Cover));
		Out.Layer[3] = static_cast<float>(std::max(100.0, Settings.FairThicknessM));
		Out.Look[0] = static_cast<float>(Settings.ExtinctionPerM);
		Out.Look[1] = static_cast<float>(GeoidAtOriginM);
		Out.Look[2] = static_cast<float>(RainCloudBaseM(Settings));
		Out.Look[3] = static_cast<float>(TimeSeconds - CloudTimePeriodS * std::floor(TimeSeconds / CloudTimePeriodS));
		// I cumuli viaggiano con lo stesso vento in quota delle nubi di pioggia.
		const double OriginNorth = (OriginLatDeg - ReferenceLatDeg) * 111132.0 - Field.GetDriftNorth();
		const double OriginEast = (OriginLonDeg - ReferenceLonDeg) * 111320.0 * std::cos(ReferenceLatDeg * WindDegToRad) - Field.GetDriftEast();
		Out.Fair[0] = static_cast<float>(OriginNorth - FairPatternPeriodM * std::floor(OriginNorth / FairPatternPeriodM));
		Out.Fair[1] = static_cast<float>(OriginEast - FairPatternPeriodM * std::floor(OriginEast / FairPatternPeriodM));
		return Out;
	}

	/** Lo strato che il motore deve attraversare (m s.l.m.): dalla base piu' bassa alla cima piu' alta. */
	inline void CloudLayerBounds(const FCloudSettings& Settings, const FPrecipitationField& Field, double& OutBottomM, double& OutTopM)
	{
		const bool bRain = Field.GetSettings().RateMmH > 0.0 && Field.GetSettings().Coverage > 0.0;
		OutBottomM = bRain ? RainCloudBaseM(Settings) : Settings.BaseM;
		OutTopM = Settings.BaseM + std::max(100.0, Settings.FairThicknessM);
		if (bRain) { OutTopM = std::max(OutTopM, Field.CloudTopM()); }
	}

	/** C'e' qualcosa da disegnare? Senza, il componente si spegne e non costa niente. */
	inline bool HasClouds(const FCloudSettings& Settings, const FPrecipitationField& Field)
	{
		return Settings.Cover > 0.0 || (Field.GetSettings().RateMmH > 0.0 && Field.GetSettings().Coverage > 0.0);
	}
}
