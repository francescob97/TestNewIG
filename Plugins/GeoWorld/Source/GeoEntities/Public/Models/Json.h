// =============================================================================
//  Json.h -- Un lettore JSON piccolo, per glTF. STRATO: C++ PURO.
//
//  glTF descrive il modello in JSON (docs/modelli-design.md, sezione 13). Lo
//  strato puro non puo' usare FJsonObject di Unreal (i test girano senza
//  Unreal) e non vuole librerie esterne: questo e' il minimo che serve, con
//  le regole di RFC 8259:
//
//    - oggetti, array, stringhe (con gli escape, \uXXXX e le coppie surrogate
//      verso UTF-8), numeri, true, false, null;
//    - i numeri si leggono a mano, non con strtod (che dipende dal "locale":
//      con il locale italiano "0.5" diventerebbe 0);
//    - una profondita' massima (un file cattivo non fa saltare lo stack);
//    - l'errore dice la riga e la colonna.
//
//  L'accesso e' tollerante: V["chiave"][3] su qualcosa che non c'e' da' un
//  valore nullo, non un'eccezione. Chi legge decide i default (come glTF).
// =============================================================================
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace GeoWorld::Models::Json
{
	enum class EType : uint8_t { Null, Bool, Number, String, Array, Object };

	struct FValue
	{
		EType Type = EType::Null;
		bool Bool = false;
		double Number = 0.0;
		std::string String;
		/** Array: gli elementi. Oggetto: i valori, nell'ordine del file (Keys accanto). */
		std::vector<FValue> Items;
		std::vector<std::string> Keys;

		bool IsNull() const { return Type == EType::Null; }
		bool IsNumber() const { return Type == EType::Number; }
		bool IsString() const { return Type == EType::String; }
		bool IsArray() const { return Type == EType::Array; }
		bool IsObject() const { return Type == EType::Object; }
		bool IsBool() const { return Type == EType::Bool; }

		/** Elementi di un array o membri di un oggetto. */
		size_t Size() const { return (Type == EType::Array || Type == EType::Object) ? Items.size() : 0; }

		const FValue* Find(const std::string& Key) const
		{
			if (Type != EType::Object) { return nullptr; }
			for (size_t I = 0; I < Keys.size(); ++I) { if (Keys[I] == Key) { return &Items[I]; } }
			return nullptr;
		}

		bool Has(const std::string& Key) const { return Find(Key) != nullptr; }

		const FValue& operator[](const std::string& Key) const
		{
			const FValue* Found = Find(Key);
			return Found ? *Found : NullValue();
		}

		const FValue& operator[](size_t Index) const
		{
			return (Type == EType::Array && Index < Items.size()) ? Items[Index] : NullValue();
		}

		double NumberOr(double Default) const { return Type == EType::Number ? Number : Default; }
		bool BoolOr(bool Default) const { return Type == EType::Bool ? Bool : Default; }
		const std::string& StringOr(const std::string& Default) const { return Type == EType::String ? String : Default; }

		/** Un intero (un indice): -1 se non c'e' o non e' un intero non negativo. */
		int64_t IndexOr(int64_t Default = -1) const
		{
			if (Type != EType::Number || Number < 0.0 || Number > 9.0e15 || std::floor(Number) != Number) { return Default; }
			return static_cast<int64_t>(Number);
		}

		static const FValue& NullValue()
		{
			static const FValue Null;
			return Null;
		}
	};

	namespace Detail
	{
		struct FParser
		{
			const char* Data = nullptr;
			size_t Size = 0;
			size_t Pos = 0;
			std::string Error;
			static constexpr int MaxDepth = 200;

			bool Fail(const std::string& What)
			{
				if (!Error.empty()) { return false; }
				size_t Line = 1, Column = 1;
				for (size_t I = 0; I < Pos && I < Size; ++I)
				{
					if (Data[I] == '\n') { ++Line; Column = 1; } else { ++Column; }
				}
				Error = "JSON, riga " + std::to_string(Line) + " colonna " + std::to_string(Column) + ": " + What;
				return false;
			}

			void SkipSpace()
			{
				while (Pos < Size && (Data[Pos] == ' ' || Data[Pos] == '\t' || Data[Pos] == '\n' || Data[Pos] == '\r')) { ++Pos; }
			}

			bool Literal(const char* Word)
			{
				size_t I = 0;
				while (Word[I] != '\0')
				{
					if (Pos + I >= Size || Data[Pos + I] != Word[I]) { return Fail(std::string("atteso '") + Word + "'"); }
					++I;
				}
				Pos += I;
				return true;
			}

			static void AppendUtf8(std::string& Out, uint32_t Code)
			{
				if (Code < 0x80) { Out += static_cast<char>(Code); }
				else if (Code < 0x800) { Out += static_cast<char>(0xC0 | (Code >> 6)); Out += static_cast<char>(0x80 | (Code & 0x3F)); }
				else if (Code < 0x10000)
				{
					Out += static_cast<char>(0xE0 | (Code >> 12));
					Out += static_cast<char>(0x80 | ((Code >> 6) & 0x3F));
					Out += static_cast<char>(0x80 | (Code & 0x3F));
				}
				else
				{
					Out += static_cast<char>(0xF0 | (Code >> 18));
					Out += static_cast<char>(0x80 | ((Code >> 12) & 0x3F));
					Out += static_cast<char>(0x80 | ((Code >> 6) & 0x3F));
					Out += static_cast<char>(0x80 | (Code & 0x3F));
				}
			}

			bool Hex4(uint32_t& Out)
			{
				Out = 0;
				if (Pos + 4 > Size) { return Fail("\\u incompleto"); }
				for (int I = 0; I < 4; ++I)
				{
					const char C = Data[Pos++];
					uint32_t Digit = 0;
					if (C >= '0' && C <= '9') { Digit = static_cast<uint32_t>(C - '0'); }
					else if (C >= 'a' && C <= 'f') { Digit = static_cast<uint32_t>(C - 'a' + 10); }
					else if (C >= 'A' && C <= 'F') { Digit = static_cast<uint32_t>(C - 'A' + 10); }
					else { return Fail("cifra esadecimale sbagliata in \\u"); }
					Out = Out * 16 + Digit;
				}
				return true;
			}

			bool ParseString(std::string& Out)
			{
				// Pos e' sulle virgolette d'apertura.
				++Pos;
				Out.clear();
				while (true)
				{
					if (Pos >= Size) { return Fail("stringa non chiusa"); }
					const char C = Data[Pos++];
					if (C == '"') { return true; }
					if (static_cast<unsigned char>(C) < 0x20) { return Fail("carattere di controllo dentro una stringa"); }
					if (C != '\\') { Out += C; continue; }
					if (Pos >= Size) { return Fail("stringa non chiusa"); }
					const char E = Data[Pos++];
					switch (E)
					{
					case '"': Out += '"'; break;
					case '\\': Out += '\\'; break;
					case '/': Out += '/'; break;
					case 'b': Out += '\b'; break;
					case 'f': Out += '\f'; break;
					case 'n': Out += '\n'; break;
					case 'r': Out += '\r'; break;
					case 't': Out += '\t'; break;
					case 'u':
					{
						uint32_t Code = 0;
						if (!Hex4(Code)) { return false; }
						// Una coppia surrogata: due \u per un carattere fuori dal piano base.
						if (Code >= 0xD800 && Code <= 0xDBFF && Pos + 6 <= Size && Data[Pos] == '\\' && Data[Pos + 1] == 'u')
						{
							Pos += 2;
							uint32_t Low = 0;
							if (!Hex4(Low)) { return false; }
							if (Low >= 0xDC00 && Low <= 0xDFFF) { Code = 0x10000 + ((Code - 0xD800) << 10) + (Low - 0xDC00); }
							else { AppendUtf8(Out, 0xFFFD); Code = Low; }
						}
						AppendUtf8(Out, Code);
						break;
					}
					default: return Fail("escape sconosciuto in una stringa");
					}
				}
			}

			bool ParseNumber(double& Out)
			{
				bool bNegative = false;
				if (Pos < Size && Data[Pos] == '-') { bNegative = true; ++Pos; }
				if (Pos >= Size || Data[Pos] < '0' || Data[Pos] > '9') { return Fail("numero sbagliato"); }
				// La mantissa come intero (le prime 19 cifre) e l'esponente decimale.
				uint64_t Mantissa = 0;
				int Digits = 0, Exponent = 0;
				const auto Digit = [&](char C, bool bFraction)
				{
					if (Digits < 19) { Mantissa = Mantissa * 10 + static_cast<uint64_t>(C - '0'); if (Mantissa > 0) { ++Digits; } if (bFraction) { --Exponent; } }
					else if (!bFraction) { ++Exponent; }
				};
				if (Data[Pos] == '0') { ++Pos; }
				else { while (Pos < Size && Data[Pos] >= '0' && Data[Pos] <= '9') { Digit(Data[Pos++], false); } }
				if (Pos < Size && Data[Pos] == '.')
				{
					++Pos;
					if (Pos >= Size || Data[Pos] < '0' || Data[Pos] > '9') { return Fail("numero sbagliato dopo il punto"); }
					while (Pos < Size && Data[Pos] >= '0' && Data[Pos] <= '9') { Digit(Data[Pos++], true); }
				}
				if (Pos < Size && (Data[Pos] == 'e' || Data[Pos] == 'E'))
				{
					++Pos;
					bool bExpNegative = false;
					if (Pos < Size && (Data[Pos] == '+' || Data[Pos] == '-')) { bExpNegative = Data[Pos] == '-'; ++Pos; }
					if (Pos >= Size || Data[Pos] < '0' || Data[Pos] > '9') { return Fail("esponente sbagliato"); }
					int Value = 0;
					while (Pos < Size && Data[Pos] >= '0' && Data[Pos] <= '9') { if (Value < 10000) { Value = Value * 10 + (Data[Pos] - '0'); } ++Pos; }
					Exponent += bExpNegative ? -Value : Value;
				}
				double Result = static_cast<double>(Mantissa);
				if (Mantissa != 0 && Exponent != 0)
				{
					// Due passi per non perdere i numeri molto piccoli o molto grandi.
					if (Exponent < -300) { Result *= std::pow(10.0, -300); Exponent += 300; }
					Result = Exponent < 0 ? Result / std::pow(10.0, -Exponent) : Result * std::pow(10.0, Exponent);
				}
				Out = bNegative ? -Result : Result;
				return true;
			}

			bool ParseValue(FValue& Out, int Depth)
			{
				if (Depth > MaxDepth) { return Fail("troppo annidato"); }
				SkipSpace();
				if (Pos >= Size) { return Fail("finito prima del valore"); }
				const char C = Data[Pos];
				if (C == '{')
				{
					Out.Type = EType::Object;
					++Pos;
					SkipSpace();
					if (Pos < Size && Data[Pos] == '}') { ++Pos; return true; }
					while (true)
					{
						SkipSpace();
						if (Pos >= Size || Data[Pos] != '"') { return Fail("attesa una chiave fra virgolette"); }
						std::string Key;
						if (!ParseString(Key)) { return false; }
						SkipSpace();
						if (Pos >= Size || Data[Pos] != ':') { return Fail("attesi i due punti"); }
						++Pos;
						Out.Keys.push_back(std::move(Key));
						Out.Items.emplace_back();
						if (!ParseValue(Out.Items.back(), Depth + 1)) { return false; }
						SkipSpace();
						if (Pos < Size && Data[Pos] == ',') { ++Pos; continue; }
						if (Pos < Size && Data[Pos] == '}') { ++Pos; return true; }
						return Fail("attesa una virgola o la graffa di chiusura");
					}
				}
				if (C == '[')
				{
					Out.Type = EType::Array;
					++Pos;
					SkipSpace();
					if (Pos < Size && Data[Pos] == ']') { ++Pos; return true; }
					while (true)
					{
						Out.Items.emplace_back();
						if (!ParseValue(Out.Items.back(), Depth + 1)) { return false; }
						SkipSpace();
						if (Pos < Size && Data[Pos] == ',') { ++Pos; continue; }
						if (Pos < Size && Data[Pos] == ']') { ++Pos; return true; }
						return Fail("attesa una virgola o la quadra di chiusura");
					}
				}
				if (C == '"') { Out.Type = EType::String; return ParseString(Out.String); }
				if (C == 't') { Out.Type = EType::Bool; Out.Bool = true; return Literal("true"); }
				if (C == 'f') { Out.Type = EType::Bool; Out.Bool = false; return Literal("false"); }
				if (C == 'n') { Out.Type = EType::Null; return Literal("null"); }
				if (C == '-' || (C >= '0' && C <= '9')) { Out.Type = EType::Number; return ParseNumber(Out.Number); }
				return Fail(std::string("carattere inatteso '") + C + "'");
			}
		};
	}

	/** Legge un documento JSON intero (spazi in fondo ammessi, altro no). */
	inline bool Parse(const char* Data, size_t Size, FValue& Out, std::string& Error)
	{
		Detail::FParser Parser;
		Parser.Data = Data;
		Parser.Size = Size;
		// Il BOM di UTF-8, che qualche esportatore scrive.
		if (Size >= 3 && static_cast<unsigned char>(Data[0]) == 0xEF && static_cast<unsigned char>(Data[1]) == 0xBB && static_cast<unsigned char>(Data[2]) == 0xBF) { Parser.Pos = 3; }
		Out = FValue{};
		if (!Parser.ParseValue(Out, 0)) { Error = Parser.Error; return false; }
		Parser.SkipSpace();
		if (Parser.Pos != Size) { Parser.Fail("testo dopo la fine del documento"); Error = Parser.Error; return false; }
		return true;
	}

	inline bool Parse(const std::string& Text, FValue& Out, std::string& Error) { return Parse(Text.data(), Text.size(), Out, Error); }
}
