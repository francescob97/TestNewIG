// =============================================================================
//  EntityModels.h -- Che cosa disegnare per un'entita'. STRATO: C++ PURO.
//
//  L'identita' dice CHI e' un'entita', il tipo (FEntityKind) dice COSA e'; il
//  modello e' un'altra cosa ancora, e la traduzione e' una TABELLA DI DATI,
//  non codice (docs/fase7-design.md, sezione 7): l'elenco dei tipi di DIS
//  cambia piu' volte l'anno, e non si ricompila il motore per un elicottero.
//
//  Il file, una regola per riga:
//
//      # tipo (7 campi, * = qualsiasi)    modello
//      1.1.105.1.*.*.*                    box 7 3.5 2.4
//      1.2.*.*.*.*.*                      plane 15 12 4
//      1.2.*.20.*.*.*                     heli 12 11 3.5
//      1.1.*.*.*.*.*                      /Game/Veicoli/SM_Camion.SM_Camion
//      cigi 12                            box 4.5 1.8 1.5
//      1.2.105.1                          F16/f16.flt yaw 90
//      1.1.105.81                         "auto rossa/auto.obj" scale 0.01 up z forward x
//      1.1.105.12                         radar/radar.glb anim ruota
//
//  - i campi mancanti in fondo valgono *: "1.2" e' "1.2.*.*.*.*.*";
//  - vince la regola PIU' SPECIFICA (piu' campi scritti); a parita', l'ULTIMA
//    (si aggiungono correzioni in fondo, come in un foglio di stile);
//  - "cigi N" e' l'indice di modello di CIGI, concordato con l'host: viene
//    prima di qualsiasi regola sul tipo;
//  - il modello: un percorso di asset di Unreal, oppure una forma fatta di
//    scatole, misure in metri:
//        box L W H     una scatola lunga L, larga W, alta H, appoggiata
//                      sull'origine (un veicolo, una nave, una persona);
//        plane L S H   un aereo: fusoliera lunga L, apertura alare S, alto H
//                      (deriva compresa), centrato sull'origine;
//        heli L R H    un elicottero: corpo lungo L, rotore di diametro R,
//                      alto H, centrato.
//
//  - oppure un FILE di modello (docs/modelli-design.md): .flt, .obj, .gltf,
//    .glb (e gli altri formati, che il caricatore dice come convertire; o
//    'geoworld convert-models' li converte), relativo alla
//    cartella dei modelli (geo.Entities.ModelFolder) o assoluto; con le
//    virgolette se ha degli spazi. Dopo, le correzioni:
//        scale S          moltiplica le misure (un modello in centimetri: 0.01)
//        yaw G            gira il modello di G gradi, positivo a destra
//        offset A D H     sposta l'origine: avanti, destra, alto (metri)
//        up X, forward X  gli assi del file (x, -x, y, -y, z, -z) quando non
//                         sono quelli del formato (OBJ: up y, forward -z)
//        anim A           un'animazione del file (glTF: nome o numero) che
//                         gira sempre, in loop (un radar, un rotore, uno che
//                         cammina)
//
//  La REGOLA DI RIPIEGO: se nessuna regola del file vale, si usa la tabella
//  di serie (una forma per dominio) e lo si DICE (bFallback): un'entita'
//  disegnata come una scatola con scritto il suo tipo e' infinitamente piu'
//  utile di una che non compare.
// =============================================================================
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

#include "Entities/EntityState.h"
#include "Models/ModelData.h"
#include "Models/ModelRig.h"

namespace GeoWorld::Entities
{
	/** Una scatola di una forma: centro e misure negli assi dell'ATTORE (X avanti, Y destra, Z alto), metri. */
	struct FBoxPart
	{
		double CenterX = 0.0, CenterY = 0.0, CenterZ = 0.0;
		double SizeX = 1.0, SizeY = 1.0, SizeZ = 1.0;
	};

	struct FModelSpec
	{
		/** Vuoto: una forma (Parts) o un file. Altrimenti il percorso dell'asset. */
		std::string Asset;
		/** Un file di modello (.flt, .obj...), come scritto nella regola; le sue correzioni. */
		std::string File;
		Models::FRigOptions FileOptions;
		/** L'animazione del file che gira sempre (nome o numero; vuoto: nessuna). */
		std::string Animation;
		std::vector<FBoxPart> Parts;
		/** Il testo della regola, per la diagnostica (geo.Entities.Models). */
		std::string Text;

		bool IsAsset() const { return !Asset.empty(); }
		bool IsFile() const { return !File.empty(); }

		/** La lunghezza massima, metri: serve a chi decide quanto e' grande a schermo. */
		double Extent() const
		{
			double Out = 1.0;
			for (const FBoxPart& Part : Parts)
			{
				Out = std::max({ Out, std::fabs(Part.CenterX) * 2.0 + Part.SizeX, std::fabs(Part.CenterY) * 2.0 + Part.SizeY,
					std::fabs(Part.CenterZ) * 2.0 + Part.SizeZ });
			}
			return Out;
		}
	};

	/** Le forme, dalle misure: le stesse che il file chiama box, plane, heli. */
	namespace Shapes
	{
		inline std::vector<FBoxPart> Box(double Length, double Width, double Height)
		{
			return { FBoxPart{ 0.0, 0.0, 0.5 * Height, Length, Width, Height } };
		}

		inline std::vector<FBoxPart> Plane(double Length, double Span, double Height)
		{
			const double Body = std::max(0.3, Height * 0.4);
			return {
				FBoxPart{ 0.0, 0.0, 0.0, Length, Body, Body },                                  // fusoliera
				FBoxPart{ 0.05 * Length, 0.0, 0.0, 0.22 * Length, Span, 0.25 * Body },          // ala
				FBoxPart{ -0.42 * Length, 0.0, 0.0, 0.12 * Length, 0.35 * Span, 0.2 * Body },   // piano di coda
				FBoxPart{ -0.42 * Length, 0.0, 0.5 * Height - 0.25 * Body, 0.14 * Length, 0.2 * Body,
					std::max(0.1, Height - 0.5 * Body) },                                          // deriva
			};
		}

		inline std::vector<FBoxPart> Heli(double Length, double Rotor, double Height)
		{
			const double Body = std::max(0.5, Height * 0.55);
			return {
				FBoxPart{ 0.15 * Length, 0.0, 0.0, 0.5 * Length, 0.3 * Body + 1.0, Body },     // cabina
				FBoxPart{ -0.3 * Length, 0.0, 0.1 * Body, 0.45 * Length, 0.3, 0.3 },         // trave di coda
				FBoxPart{ 0.1 * Length, 0.0, 0.5 * Height, Rotor, Rotor, 0.05 },             // disco del rotore
			};
		}
	}

	struct FModelRule
	{
		/** I sette campi; -1 = qualsiasi. */
		int32_t Fields[7] = { -1, -1, -1, -1, -1, -1, -1 };
		/** >= 0: una regola "cigi N" (sull'indice di modello, non sul tipo). */
		int32_t CigiModel = -1;
		FModelSpec Model;
		/** Dalla tabella di serie (il ripiego). */
		bool bBuiltIn = false;
		int32_t Line = 0;

		int32_t Specificity() const
		{
			int32_t Count = 0;
			for (int32_t Value : Fields) { Count += Value >= 0 ? 1 : 0; }
			return Count;
		}

		bool Matches(const FEntityKind& Kind) const
		{
			if (CigiModel >= 0) { return false; }
			for (int32_t Index = 0; Index < 7; ++Index)
			{
				if (Fields[Index] >= 0 && Fields[Index] != Kind.Field(Index)) { return false; }
			}
			return true;
		}

		std::string PatternText() const
		{
			if (CigiModel >= 0) { return "cigi " + std::to_string(CigiModel); }
			std::string Out;
			for (int32_t Index = 0; Index < 7; ++Index)
			{
				if (Index > 0) { Out += '.'; }
				Out += Fields[Index] >= 0 ? std::to_string(Fields[Index]) : std::string("*");
			}
			return Out;
		}
	};

	struct FModelMatch
	{
		const FModelRule* Rule = nullptr;
		/** Nessuna regola del file: e' la forma di serie del dominio. Si dice a schermo. */
		bool bFallback = true;
	};

	struct FModelTableError
	{
		int32_t Line = 0;
		std::string Message;
	};

	class FModelTable
	{
	public:
		FModelTable() { AddBuiltIns(); }

		/**
		 * Legge il file. Le righe sbagliate si saltano e si elencano in Errors
		 * (con il numero di riga): una riga sbagliata non butta via le altre.
		 * Le regole di serie restano sempre, sotto.
		 */
		bool Parse(const std::string& Text, std::vector<FModelTableError>& Errors)
		{
			Rules.clear();
			AddBuiltIns();
			int32_t LineNumber = 0;
			size_t Start = 0;
			while (Start <= Text.size())
			{
				const size_t End = Text.find('\n', Start);
				std::string Line = Text.substr(Start, End == std::string::npos ? std::string::npos : End - Start);
				++LineNumber;
				Start = End == std::string::npos ? Text.size() + 1 : End + 1;

				const size_t Hash = Line.find('#');
				if (Hash != std::string::npos) { Line.resize(Hash); }
				std::vector<std::string> Words = SplitWords(Line);
				if (Words.empty()) { continue; }

				FModelRule Rule;
				Rule.Line = LineNumber;
				size_t First = 1;
				std::string Error;
				if (Words[0] == "cigi")
				{
					if (Words.size() < 2 || !ParseInt(Words[1], 0, 65535, Rule.CigiModel))
					{
						Errors.push_back({ LineNumber, "dopo 'cigi' ci vuole l'indice del modello (0..65535)" });
						continue;
					}
					First = 2;
				}
				else if (!ParsePattern(Words[0], Rule, Error))
				{
					Errors.push_back({ LineNumber, Error });
					continue;
				}
				if (Words.size() <= First)
				{
					Errors.push_back({ LineNumber, "manca il modello dopo '" + Words[0] + "'" });
					continue;
				}
				std::vector<std::string> ModelWords(Words.begin() + static_cast<std::ptrdiff_t>(First), Words.end());
				if (!ParseModel(ModelWords, Rule.Model, Error))
				{
					Errors.push_back({ LineNumber, Error });
					continue;
				}
				Rules.push_back(std::move(Rule));
			}
			return Errors.empty();
		}

		/** Il modello di un'entita': prima "cigi N" (se ModelId >= 0), poi la regola piu' specifica, poi il ripiego. */
		FModelMatch Find(const FEntityKind& Kind, int32_t ModelId = -1) const
		{
			FModelMatch Out;
			if (ModelId >= 0)
			{
				for (auto It = Rules.rbegin(); It != Rules.rend(); ++It)
				{
					if (It->CigiModel == ModelId) { Out.Rule = &*It; Out.bFallback = It->bBuiltIn; return Out; }
				}
			}
			int32_t Best = -1;
			for (const FModelRule& Rule : Rules)
			{
				if (!Rule.Matches(Kind)) { continue; }
				// Prima chi e' del file, poi la specificita', poi l'ultima.
				const int32_t Score = (Rule.bBuiltIn ? 0 : 100) + Rule.Specificity();
				if (Score >= Best) { Best = Score; Out.Rule = &Rule; }
			}
			Out.bFallback = Out.Rule == nullptr || Out.Rule->bBuiltIn;
			return Out;
		}

		const std::vector<FModelRule>& All() const { return Rules; }
		size_t NumFromFile() const
		{
			size_t Count = 0;
			for (const FModelRule& Rule : Rules) { Count += Rule.bBuiltIn ? 0 : 1; }
			return Count;
		}

		/** Una forma dal testo ("box 4.5 1.8 1.5", un asset). Pubblica: la usa anche chi fa i test. */
		static bool ParseModel(const std::vector<std::string>& Words, FModelSpec& Out, std::string& Error)
		{
			Out = FModelSpec{};
			for (const std::string& Word : Words) { Out.Text += (Out.Text.empty() ? "" : " ") + Word; }
			if (Words.empty()) { Error = "modello vuoto"; return false; }
			if (Words[0] == "box" || Words[0] == "plane" || Words[0] == "heli")
			{
				double Size[3] = { 0.0, 0.0, 0.0 };
				if (Words.size() != 4)
				{
					Error = "'" + Words[0] + "' vuole tre misure in metri (lunghezza, larghezza o apertura, altezza)";
					return false;
				}
				for (size_t Index = 0; Index < 3; ++Index)
				{
					char* EndPtr = nullptr;
					const std::string& Field = Words[1 + Index];
					Size[Index] = std::strtod(Field.c_str(), &EndPtr);
					if (EndPtr == Field.c_str() || *EndPtr != '\0' || !(Size[Index] > 0.0) || Size[Index] > 2000.0)
					{
						Error = "misura non valida: '" + Field + "' (metri, fra 0 e 2000)";
						return false;
					}
				}
				Out.Parts = Words[0] == "box" ? Shapes::Box(Size[0], Size[1], Size[2])
					: Words[0] == "plane" ? Shapes::Plane(Size[0], Size[1], Size[2])
					: Shapes::Heli(Size[0], Size[1], Size[2]);
				return true;
			}
			if (IsModelFileName(Words[0]))
			{
				Out.File = Words[0];
				return ParseFileOptions(Words, Out.FileOptions, Out.Animation, Error);
			}
			if (Words[0].front() == '/')
			{
				// /Game/..., /Engine/..., /<Plugin>/...: il percorso di un asset.
				if (Words.size() != 1) { Error = "dopo il percorso dell'asset non ci va altro"; return false; }
				Out.Asset = Words[0];
				return true;
			}
			Error = "modello sconosciuto '" + Words[0] + "': box, plane, heli, un file (.flt, .obj, .gltf, .glb) o un percorso /Game/...";
			return false;
		}

		/** Un nome di file di modello: l'estensione lo dice (.flt, .obj, .gltf, .fbx, .ive...). */
		static bool IsModelFileName(const std::string& Word)
		{
			const size_t Dot = Word.find_last_of('.');
			const size_t Slash = Word.find_last_of("/\\");
			if (Dot == std::string::npos || (Slash != std::string::npos && Dot < Slash)) { return false; }
			std::string Extension = Word.substr(Dot + 1);
			for (char& C : Extension) { C = static_cast<char>(std::tolower(static_cast<unsigned char>(C))); }
			static const char* Known[] = { "flt", "obj", "gltf", "glb", "fbx", "ive", "osg", "osgb", "osgt", "3ds", "dae", "lwo", "stl", "ply", "blend" };
			for (const char* K : Known) { if (Extension == K) { return true; } }
			return false;
		}

		/** Le correzioni dopo il nome del file: scale, yaw, offset, up, forward; e anim. */
		static bool ParseFileOptions(const std::vector<std::string>& Words, Models::FRigOptions& Out, std::string& OutAnimation, std::string& Error)
		{
			Out = Models::FRigOptions{};
			OutAnimation.clear();
			bool bUp = false, bForward = false;
			const auto Number = [&](size_t At, double& Value)
			{
				if (At >= Words.size()) { return false; }
				char* EndPtr = nullptr;
				Value = std::strtod(Words[At].c_str(), &EndPtr);
				return EndPtr != Words[At].c_str() && *EndPtr == '\0' && std::isfinite(Value);
			};
			for (size_t I = 1; I < Words.size();)
			{
				const std::string& Key = Words[I];
				if (Key == "scale")
				{
					if (!Number(I + 1, Out.Scale) || !(Out.Scale > 0.0)) { Error = "'scale' vuole un numero maggiore di zero"; return false; }
					I += 2;
				}
				else if (Key == "yaw")
				{
					if (!Number(I + 1, Out.YawDeg)) { Error = "'yaw' vuole i gradi"; return false; }
					I += 2;
				}
				else if (Key == "offset")
				{
					if (!Number(I + 1, Out.Offset[0]) || !Number(I + 2, Out.Offset[1]) || !Number(I + 3, Out.Offset[2]))
					{
						Error = "'offset' vuole tre numeri: avanti, destra, alto (metri)";
						return false;
					}
					I += 4;
				}
				else if (Key == "up" || Key == "forward")
				{
					Models::EAxis Axis;
					if (I + 1 >= Words.size() || !Models::ParseAxis(Words[I + 1], Axis))
					{
						Error = "'" + Key + "' vuole un asse: x, -x, y, -y, z, -z";
						return false;
					}
					(Key == "up" ? Out.Axes.Up : Out.Axes.Forward) = Axis;
					(Key == "up" ? bUp : bForward) = true;
					I += 2;
				}
				else if (Key == "anim")
				{
					if (I + 1 >= Words.size()) { Error = "'anim' vuole il nome (o il numero) di un'animazione del file"; return false; }
					OutAnimation = Words[I + 1];
					I += 2;
				}
				else
				{
					Error = "dopo il file: scale, yaw, offset, up, forward, anim (non '" + Key + "')";
					return false;
				}
			}
			if (bUp != bForward) { Error = "'up' e 'forward' vanno insieme"; return false; }
			if (bUp && !Out.Axes.IsValid()) { Error = "'up' e 'forward' sullo stesso asse"; return false; }
			Out.bOverrideAxes = bUp;
			return true;
		}

	private:
		std::vector<FModelRule> Rules;

		void AddBuiltIn(std::initializer_list<int32_t> Pattern, const char* Model)
		{
			FModelRule Rule;
			int32_t Index = 0;
			for (int32_t Value : Pattern) { Rule.Fields[Index++] = Value; }
			std::string Error;
			ParseModel(SplitWords(Model), Rule.Model, Error);
			Rule.bBuiltIn = true;
			Rules.push_back(std::move(Rule));
		}

		/** La tabella di serie: una forma per dominio, misure da mezzo tipico. */
		void AddBuiltIns()
		{
			AddBuiltIn({}, "box 3 3 3");                  // qualsiasi cosa
			AddBuiltIn({ 1, 1 }, "box 4.5 1.8 1.5");      // terra: un'auto
			AddBuiltIn({ 1, 2 }, "plane 15 12 4");        // aria: un aereo
			AddBuiltIn({ 1, 3 }, "box 30 8 6");           // superficie: una barca grande
			AddBuiltIn({ 1, 4 }, "box 60 8 8");           // sottomarino
			AddBuiltIn({ 1, 5 }, "box 5 5 5");            // spazio
			AddBuiltIn({ 3 }, "box 0.5 0.5 1.8");         // forma di vita: una persona
			AddBuiltIn({ 2 }, "box 3 0.4 0.4");           // munizione
		}

		static std::vector<std::string> SplitWords(const std::string& Line)
		{
			// Le parole; fra virgolette anche con gli spazi (i percorsi).
			std::vector<std::string> Words;
			std::string Word;
			bool bQuoted = false, bHad = false;
			for (char C : Line)
			{
				if (C == '"') { bQuoted = !bQuoted; bHad = true; continue; }
				if (!bQuoted && std::isspace(static_cast<unsigned char>(C)))
				{
					if (!Word.empty() || bHad) { Words.push_back(Word); Word.clear(); bHad = false; }
				}
				else { Word += C; }
			}
			if (!Word.empty() || bHad) { Words.push_back(Word); }
			return Words;
		}

		static bool ParseInt(const std::string& Text, int32_t Min, int32_t Max, int32_t& Out)
		{
			if (Text.empty() || Text.size() > 6) { return false; }
			for (char C : Text) { if (!std::isdigit(static_cast<unsigned char>(C))) { return false; } }
			const long Value = std::strtol(Text.c_str(), nullptr, 10);
			if (Value < Min || Value > Max) { return false; }
			Out = static_cast<int32_t>(Value);
			return true;
		}

		static bool ParsePattern(const std::string& Text, FModelRule& Rule, std::string& Error)
		{
			static constexpr int32_t Limits[7] = { 255, 255, 65535, 255, 255, 255, 255 };
			int32_t Index = 0;
			size_t Start = 0;
			while (true)
			{
				const size_t Dot = Text.find('.', Start);
				const std::string Field = Text.substr(Start, Dot == std::string::npos ? std::string::npos : Dot - Start);
				if (Index >= 7) { Error = "il tipo ha al massimo sette campi: '" + Text + "'"; return false; }
				if (Field != "*" && !ParseInt(Field, 0, Limits[Index], Rule.Fields[Index]))
				{
					Error = "campo " + std::to_string(Index + 1) + " non valido in '" + Text + "' (un numero o *)";
					return false;
				}
				++Index;
				if (Dot == std::string::npos) { break; }
				Start = Dot + 1;
			}
			return true;
		}
	};
}
