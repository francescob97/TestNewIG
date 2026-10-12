// =============================================================================
//  WaterTextures.h -- Le texture dell'acqua, generate dal codice.
//  STRATO: C++ PURO, header-only.
//
//  Come l'atlante delle strade: niente file binari da mantenere, e ogni pixel
//  si prova nei test.
//
//  LA NORMALE DI DETTAGLIO. Le onde di Gerstner della geometria sono lunghe
//  decine di metri; l'increspatura di pochi centimetri che fa luccicare il
//  sole sull'acqua sta nella normale. Si genera sommando onde con frequenze
//  INTERE sul lato della texture: una sinusoide che fa un numero intero di
//  giri in 256 pixel finisce dove comincia, quindi la texture si ripete
//  senza cuciture. Le ampiezze calano con la frequenza come in uno spettro di
//  mare (piu' corta, piu' bassa).
//
//  LA SCHIUMA. Rumore cellulare (Worley): la distanza dal "seme" piu' vicino
//  disegna celle come bolle. I semi stanno su una griglia che si richiude sui
//  bordi, e anche questa si ripete senza cuciture.
//
//  Entrambe sono DATI, non colori: si creano senza sRGB.
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace GeoWorld::Water
{
	inline constexpr int32_t WaterTextureSize = 256;

	namespace Detail
	{
		/** Numero pseudo-casuale deterministico 0..1 da due interi e un seme. */
		inline double Hash01(int32_t A, int32_t B, uint32_t Seed)
		{
			uint32_t H = static_cast<uint32_t>(A) * 374761393u + static_cast<uint32_t>(B) * 668265263u + Seed * 2246822519u;
			H = (H ^ (H >> 13)) * 1274126177u;
			H ^= H >> 16;
			return static_cast<double>(H & 0xFFFFFF) / static_cast<double>(0xFFFFFF);
		}

		inline uint8_t ToByte(double Value01)
		{
			return static_cast<uint8_t>(std::min(255.0, std::max(0.0, std::round(Value01 * 255.0))));
		}
	}

	/**
	 * La normale di dettaglio: Size x Size BGRA lineare. R, G = componenti x,
	 * y della normale in [0,1] (0,5 = piatto), B = z, A = altezza (0..1).
	 */
	inline void BuildWaterNormalTexture(std::vector<uint8_t>& OutBGRA, int32_t Size = WaterTextureSize,
	                                    uint32_t Seed = 7)
	{
		struct FRipple { int32_t A, B; double Amplitude, Phase; };
		std::vector<FRipple> Ripples;
		// Frequenze da 3 a 40 giri per lato, in tutte le direzioni.
		for (int32_t Index = 0; Index < 48; ++Index)
		{
			const double Radius = 3.0 + 37.0 * std::pow(Detail::Hash01(Index, 1, Seed), 1.6);
			const double Angle = 6.283185307179586 * Detail::Hash01(Index, 2, Seed);
			const int32_t A = static_cast<int32_t>(std::lround(Radius * std::cos(Angle)));
			const int32_t B = static_cast<int32_t>(std::lround(Radius * std::sin(Angle)));
			if (A == 0 && B == 0) { continue; }
			const double K = std::sqrt(static_cast<double>(A * A + B * B));
			Ripples.push_back(FRipple{ A, B, 1.0 / std::pow(K, 1.5), 6.283185307179586 * Detail::Hash01(Index, 3, Seed) });
		}

		const int32_t Pixels = Size * Size;
		std::vector<double> Height(static_cast<size_t>(Pixels)), Dx(static_cast<size_t>(Pixels)), Dy(static_cast<size_t>(Pixels));
		double MaxSlope = 1e-9, MinHeight = 1e9, MaxHeight = -1e9;
		for (int32_t Y = 0; Y < Size; ++Y)
		{
			for (int32_t X = 0; X < Size; ++X)
			{
				double H = 0.0, Gx = 0.0, Gy = 0.0;
				for (const FRipple& Ripple : Ripples)
				{
					const double Theta = 6.283185307179586 * (Ripple.A * X + Ripple.B * Y) / Size + Ripple.Phase;
					H += Ripple.Amplitude * std::sin(Theta);
					const double Slope = Ripple.Amplitude * std::cos(Theta) * 6.283185307179586 / Size;
					Gx += Slope * Ripple.A;
					Gy += Slope * Ripple.B;
				}
				const size_t Index = static_cast<size_t>(Y) * Size + X;
				Height[Index] = H;
				Dx[Index] = Gx;
				Dy[Index] = Gy;
				MaxSlope = std::max(MaxSlope, std::max(std::fabs(Gx), std::fabs(Gy)));
				MinHeight = std::min(MinHeight, H);
				MaxHeight = std::max(MaxHeight, H);
			}
		}

		// Le pendenze si scalano perche' la piu' ripida valga ~45 gradi: il
		// materiale decide poi quanto usarne.
		const double Scale = 1.0 / MaxSlope;
		OutBGRA.assign(static_cast<size_t>(Pixels) * 4, 0);
		for (int32_t Index = 0; Index < Pixels; ++Index)
		{
			double Nx = -Dx[static_cast<size_t>(Index)] * Scale, Ny = -Dy[static_cast<size_t>(Index)] * Scale, Nz = 1.0;
			const double Length = std::sqrt(Nx * Nx + Ny * Ny + Nz * Nz);
			Nx /= Length; Ny /= Length; Nz /= Length;
			uint8_t* Pixel = OutBGRA.data() + static_cast<size_t>(Index) * 4;
			Pixel[2] = Detail::ToByte(0.5 + 0.5 * Nx);
			Pixel[1] = Detail::ToByte(0.5 + 0.5 * Ny);
			Pixel[0] = Detail::ToByte(0.5 + 0.5 * Nz);
			Pixel[3] = Detail::ToByte((Height[static_cast<size_t>(Index)] - MinHeight) / std::max(1e-9, MaxHeight - MinHeight));
		}
	}

	/** La schiuma: Size x Size BGRA lineare, la stessa quantita' in R, G, B (0 = niente, 1 = piena). */
	inline void BuildWaterFoamTexture(std::vector<uint8_t>& OutBGRA, int32_t Size = WaterTextureSize, uint32_t Seed = 11)
	{
		const int32_t Pixels = Size * Size;
		OutBGRA.assign(static_cast<size_t>(Pixels) * 4, 255);

		// Due ottave di celle: bolle grandi (16 per lato) e piccole (40).
		auto Worley = [Size, Seed](double X, double Y, int32_t Cells, uint32_t Octave)
		{
			const double CellSize = static_cast<double>(Size) / Cells;
			const int32_t Cx = static_cast<int32_t>(X / CellSize), Cy = static_cast<int32_t>(Y / CellSize);
			double Nearest = 1e9;
			for (int32_t Oy = -1; Oy <= 1; ++Oy)
			{
				for (int32_t Ox = -1; Ox <= 1; ++Ox)
				{
					// La cella vicina, richiusa sui bordi: la texture si ripete.
					const int32_t Nx = ((Cx + Ox) % Cells + Cells) % Cells;
					const int32_t Ny = ((Cy + Oy) % Cells + Cells) % Cells;
					const double Px = (Cx + Ox + Detail::Hash01(Nx, Ny, Seed + Octave)) * CellSize;
					const double Py = (Cy + Oy + Detail::Hash01(Ny, Nx, Seed + 17 * Octave)) * CellSize;
					Nearest = std::min(Nearest, std::hypot(X - Px, Y - Py) / CellSize);
				}
			}
			return Nearest;   // 0 al seme, ~0,7 al bordo della cella
		};

		for (int32_t Y = 0; Y < Size; ++Y)
		{
			for (int32_t X = 0; X < Size; ++X)
			{
				const double Large = Worley(X + 0.5, Y + 0.5, 16, 1);
				const double Small = Worley(X + 0.5, Y + 0.5, 40, 2);
				// I bordi fra le bolle sono la schiuma: chiari dove la distanza e' alta.
				const double Foam = std::min(1.0, std::max(0.0, 0.65 * std::pow(std::min(1.0, Large / 0.7), 2.0)
				                                             + 0.45 * std::pow(std::min(1.0, Small / 0.7), 3.0) - 0.1));
				uint8_t* Pixel = OutBGRA.data() + (static_cast<size_t>(Y) * Size + X) * 4;
				Pixel[0] = Pixel[1] = Pixel[2] = Detail::ToByte(Foam);
			}
		}
	}
}
