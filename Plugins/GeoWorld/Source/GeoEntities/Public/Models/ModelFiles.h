// =============================================================================
//  ModelFiles.h -- I file dei modelli: leggerli, e trovare le texture.
//  STRATO: C++ PURO.
//
//  I lettori non aprono file da soli: ricevono un FFileAccess (due funzioni,
//  leggere ed esistere). In Unreal sono quelle di IFileManager; nei test una
//  cartella finta in memoria. Cosi' un modello con i suoi riferimenti esterni
//  e le sue texture si prova senza toccare il disco.
//
//  LE TEXTURE NON SONO DOVE DICE IL MODELLO. Un .flt fatto su un'altra
//  macchina dice "D:\Modelli\Texture\f16.rgb", o "/usr/people/mario/tex/f16.rgb".
//  Si cerca, in ordine:
//    1. il percorso cosi' com'e' (se esiste);
//    2. relativo alla cartella del modello;
//    3. il solo nome, accanto al modello, poi in textures/, Textures/, tex/;
//    4. il solo nome nella cartella sopra;
//    5. il solo nome in tutta la cartella dei modelli (l'indice per nome,
//       senza maiuscole, che costruisce chi chiama).
// =============================================================================
#pragma once

#include <cctype>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace GeoWorld::Models
{
	struct FFileAccess
	{
		std::function<bool(const std::string& Path, std::vector<uint8_t>& Out)> Read;
		std::function<bool(const std::string& Path)> Exists;
	};

	/** Le barre tutte uguali (/), senza doppioni. */
	inline std::string NormalisePath(std::string Path)
	{
		for (char& C : Path) { if (C == '\\') { C = '/'; } }
		std::string Out;
		Out.reserve(Path.size());
		for (size_t I = 0; I < Path.size(); ++I)
		{
			// "//" all'inizio resta (percorsi di rete \\server\cartella).
			if (Path[I] == '/' && I > 1 && Out.size() > 0 && Out.back() == '/') { continue; }
			Out += Path[I];
		}
		return Out;
	}

	inline std::string DirectoryOf(const std::string& Path)
	{
		const std::string P = NormalisePath(Path);
		const size_t Slash = P.find_last_of('/');
		return Slash == std::string::npos ? std::string() : P.substr(0, Slash);
	}

	inline std::string FileNameOf(const std::string& Path)
	{
		const std::string P = NormalisePath(Path);
		const size_t Slash = P.find_last_of('/');
		return Slash == std::string::npos ? P : P.substr(Slash + 1);
	}

	inline std::string LowerCase(std::string Text)
	{
		for (char& C : Text) { C = static_cast<char>(std::tolower(static_cast<unsigned char>(C))); }
		return Text;
	}

	/** L'estensione senza punto, minuscola ("flt"). */
	inline std::string ExtensionOf(const std::string& Path)
	{
		const std::string Name = FileNameOf(Path);
		const size_t Dot = Name.find_last_of('.');
		return Dot == std::string::npos ? std::string() : LowerCase(Name.substr(Dot + 1));
	}

	/** Assoluto: /x, \\server, C:/x. */
	inline bool IsAbsolutePath(const std::string& Path)
	{
		const std::string P = NormalisePath(Path);
		return (!P.empty() && P[0] == '/') || (P.size() > 2 && P[1] == ':' && P[2] == '/');
	}

	inline std::string JoinPath(const std::string& Directory, const std::string& Relative)
	{
		if (Directory.empty() || IsAbsolutePath(Relative)) { return NormalisePath(Relative); }
		std::string Out = NormalisePath(Directory);
		if (Out.back() != '/') { Out += '/'; }
		std::string R = NormalisePath(Relative);
		while (R.rfind("./", 0) == 0) { R = R.substr(2); }
		// "../" toglie una cartella, finche' ce n'e'.
		while (R.rfind("../", 0) == 0)
		{
			const std::string Parent = DirectoryOf(Out.substr(0, Out.size() - 1));
			if (Parent.empty()) { break; }
			Out = Parent + "/";
			R = R.substr(3);
		}
		return Out + R;
	}

	/** Dove trovare un file che il modello nomina (una texture, un riferimento esterno). Vuoto se non c'e'. */
	inline std::string ResolveReferencedFile(const std::string& Raw, const std::string& ModelDirectory, const FFileAccess& Files,
		const std::unordered_map<std::string, std::string>* IndexByName = nullptr)
	{
		if (Raw.empty() || !Files.Exists) { return std::string(); }
		const std::string Path = NormalisePath(Raw);
		if (IsAbsolutePath(Path) && Files.Exists(Path)) { return Path; }
		if (!IsAbsolutePath(Path))
		{
			const std::string Relative = JoinPath(ModelDirectory, Path);
			if (Files.Exists(Relative)) { return Relative; }
		}
		const std::string Name = FileNameOf(Path);
		const std::string Parent = DirectoryOf(ModelDirectory);
		const std::string Candidates[] = {
			JoinPath(ModelDirectory, Name), JoinPath(ModelDirectory, "textures/" + Name), JoinPath(ModelDirectory, "Textures/" + Name),
			JoinPath(ModelDirectory, "tex/" + Name), Parent.empty() ? std::string() : JoinPath(Parent, Name),
			Parent.empty() ? std::string() : JoinPath(Parent, "textures/" + Name) };
		for (const std::string& Candidate : Candidates)
		{
			if (!Candidate.empty() && Files.Exists(Candidate)) { return Candidate; }
		}
		if (IndexByName)
		{
			const auto It = IndexByName->find(LowerCase(Name));
			if (It != IndexByName->end()) { return It->second; }
		}
		return std::string();
	}
}
