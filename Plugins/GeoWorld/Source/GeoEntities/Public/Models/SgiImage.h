// =============================================================================
//  SgiImage.h -- Le immagini SGI (.rgb, .rgba, .int, .inta, .sgi, .bw): le
//  texture classiche dei modelli OpenFlight. STRATO: C++ PURO.
//
//  Il formato delle stazioni Silicon Graphics, dove OpenFlight e' nato. Unreal
//  non lo conosce; e' semplice:
//
//    intestazione 512 byte, big-endian:
//       0  uint16 474 (il numero magico)
//       2  uint8  memorizzazione: 0 cosi' com'e', 1 RLE
//       3  uint8  byte per canale: 1 (o 2: qui si tiene il byte alto)
//       4  uint16 dimensioni (1, 2 o 3)
//       6  uint16 larghezza, 8 altezza, 10 canali
//    poi i canali uno dopo l'altro (prima tutto il rosso, poi il verde...),
//    le righe dal BASSO verso l'alto.
//
//    RLE: due tabelle (inizio e lunghezza di ogni riga di ogni canale), poi
//    le righe: un byte di controllo, i 7 bit bassi sono quanti; con il bit
//    alto acceso seguono altrettanti byte da copiare, spento un byte da
//    ripetere; 0 chiude la riga.
//
//  Il risultato e' RGBA a 8 bit, righe dall'ALTO (come un PNG decodificato):
//  le immagini di tutti i formati arrivano a Unreal nello stesso verso.
//  1 canale: grigio; 2: grigio + alfa; 3: RGB; 4: RGBA.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace GeoWorld::Models
{
	struct FDecodedImage
	{
		int32_t Width = 0, Height = 0;
		/** RGBA, righe dall'alto. */
		std::vector<uint8_t> Rgba;
		/** C'e' almeno un pixel non opaco: il materiale deve tagliare o fondere. */
		bool bHasAlpha = false;
	};

	inline bool IsSgiImage(const std::vector<uint8_t>& Bytes)
	{
		return Bytes.size() >= 512 && Bytes[0] == 0x01 && Bytes[1] == 0xDA;
	}

	inline bool DecodeSgiImage(const std::vector<uint8_t>& Bytes, FDecodedImage& Out, std::string& Error)
	{
		Out = FDecodedImage{};
		if (!IsSgiImage(Bytes)) { Error = "non e' un'immagine SGI"; return false; }
		const auto U16 = [&](size_t At) { return static_cast<uint32_t>(Bytes[At] << 8 | Bytes[At + 1]); };
		const auto U32 = [&](size_t At)
		{
			return static_cast<uint32_t>(Bytes[At]) << 24 | static_cast<uint32_t>(Bytes[At + 1]) << 16
				| static_cast<uint32_t>(Bytes[At + 2]) << 8 | static_cast<uint32_t>(Bytes[At + 3]);
		};
		const uint32_t Storage = Bytes[2];
		const uint32_t BytesPerChannel = Bytes[3];
		const uint32_t Dimension = U16(4);
		const uint32_t Width = U16(6);
		const uint32_t Height = Dimension >= 2 ? U16(8) : 1;
		const uint32_t Channels = Dimension >= 3 ? U16(10) : 1;
		if (Storage > 1 || (BytesPerChannel != 1 && BytesPerChannel != 2) || Width == 0 || Height == 0 || Channels == 0 || Channels > 4
			|| Width > 16384 || Height > 16384)
		{
			Error = "intestazione SGI non valida";
			return false;
		}

		// I canali, piani, righe dal basso.
		std::vector<uint8_t> Planes(static_cast<size_t>(Width) * Height * Channels, 0);
		const auto Row = [&](uint32_t Channel, uint32_t Y) { return Planes.data() + (static_cast<size_t>(Channel) * Height + Y) * Width; };
		if (Storage == 0)
		{
			const size_t Need = 512 + static_cast<size_t>(Width) * Height * Channels * BytesPerChannel;
			if (Bytes.size() < Need) { Error = "immagine SGI troncata"; return false; }
			size_t At = 512;
			for (uint32_t Channel = 0; Channel < Channels; ++Channel)
			{
				for (uint32_t Y = 0; Y < Height; ++Y)
				{
					uint8_t* Line = Row(Channel, Y);
					for (uint32_t X = 0; X < Width; ++X, At += BytesPerChannel) { Line[X] = Bytes[At]; }
				}
			}
		}
		else
		{
			const size_t Lines = static_cast<size_t>(Height) * Channels;
			if (Bytes.size() < 512 + Lines * 8) { Error = "tabelle RLE troncate"; return false; }
			for (uint32_t Channel = 0; Channel < Channels; ++Channel)
			{
				for (uint32_t Y = 0; Y < Height; ++Y)
				{
					const size_t Entry = static_cast<size_t>(Channel) * Height + Y;
					size_t At = U32(512 + Entry * 4);
					uint8_t* Line = Row(Channel, Y);
					uint32_t X = 0;
					while (true)
					{
						if (At + BytesPerChannel > Bytes.size()) { Error = "riga RLE troncata"; return false; }
						const uint32_t Control = Bytes[At + BytesPerChannel - 1];
						At += BytesPerChannel;
						const uint32_t Count = Control & 0x7Fu;
						if (Count == 0) { break; }
						if (X + Count > Width) { Error = "riga RLE piu' lunga dell'immagine"; return false; }
						if (Control & 0x80u)
						{
							if (At + static_cast<size_t>(Count) * BytesPerChannel > Bytes.size()) { Error = "riga RLE troncata"; return false; }
							for (uint32_t I = 0; I < Count; ++I, At += BytesPerChannel) { Line[X++] = Bytes[At]; }
						}
						else
						{
							if (At + BytesPerChannel > Bytes.size()) { Error = "riga RLE troncata"; return false; }
							const uint8_t Value = Bytes[At];
							At += BytesPerChannel;
							for (uint32_t I = 0; I < Count; ++I) { Line[X++] = Value; }
						}
					}
				}
			}
		}

		Out.Width = static_cast<int32_t>(Width);
		Out.Height = static_cast<int32_t>(Height);
		Out.Rgba.assign(static_cast<size_t>(Width) * Height * 4, 255);
		for (uint32_t Y = 0; Y < Height; ++Y)
		{
			// Dall'alto: la riga Y dell'uscita e' la riga Height-1-Y del file.
			const uint32_t Source = Height - 1 - Y;
			for (uint32_t X = 0; X < Width; ++X)
			{
				uint8_t* P = Out.Rgba.data() + (static_cast<size_t>(Y) * Width + X) * 4;
				const auto Sample = [&](uint32_t Channel) { return Row(Channel, Source)[X]; };
				if (Channels <= 2)
				{
					P[0] = P[1] = P[2] = Sample(0);
					if (Channels == 2) { P[3] = Sample(1); }
				}
				else
				{
					P[0] = Sample(0); P[1] = Sample(1); P[2] = Sample(2);
					if (Channels == 4) { P[3] = Sample(3); }
				}
				if (P[3] < 255) { Out.bHasAlpha = true; }
			}
		}
		return true;
	}
}
