/*	-------------------------------------------------------------------------------------------------------
	© 1991-2012 Take-Two Interactive Software and its subsidiaries.  Developed by Firaxis Games.  
	Sid Meier's Civilization V, Civ, Civilization, 2K Games, Firaxis Games, Take-Two Interactive Software 
	and their respective logos are all trademarks of Take-Two interactive Software, Inc.  
	All other marks and trademarks are the property of their respective owners.  
	All rights reserved. 
	------------------------------------------------------------------------------------------------------- */
#pragma once

#ifndef CIV5_DANGER_PLOTS_H
#define CIV5_DANGER_PLOTS_H

#include <vector>
#include <set>
#include <new>
#include "CvEnums.h"

// A minimal vector for trivially copyable elements that keeps the first N elements inside the object and only
// allocates heap storage when it grows beyond that. clear() releases any heap storage again.
// Each player keeps three of these for every plot of the map. With std::vector, every list that was ever written
// kept its own small heap block for the rest of the game (hundreds of thousands of blocks on large maps), which
// fragments the 32-bit address space. Most lists are empty and most of the others hold a single entry, so with
// N = 1 the danger map needs no heap blocks for them and each plot entry keeps the size it had with std::vector.
// Elements are kept in insertion order, exactly as std::vector does.
template<typename T, unsigned int N>
class CvInlineVector
{
public:
	typedef T value_type;
	typedef T* iterator;
	typedef const T* const_iterator;
	typedef unsigned int size_type;

	CvInlineVector() : m_uSize(0), m_uCapacity(N) {}
	CvInlineVector(const CvInlineVector& other) : m_uSize(0), m_uCapacity(N) { append(other); }
	~CvInlineVector() { clear(); }

	CvInlineVector& operator=(const CvInlineVector& other)
	{
		if (this != &other)
		{
			clear();
			append(other);
		}
		return *this;
	}

	iterator begin() { return data(); }
	iterator end() { return data() + m_uSize; }
	const_iterator begin() const { return data(); }
	const_iterator end() const { return data() + m_uSize; }

	size_type size() const { return m_uSize; }
	bool empty() const { return m_uSize == 0; }

	void push_back(const T& value)
	{
		const T copy(value); // value may refer to an element that grow() moves
		if (m_uSize == m_uCapacity)
			grow(m_uCapacity * 2 > 4 ? m_uCapacity * 2 : 4);
		new (data() + m_uSize) T(copy);
		m_uSize++;
	}

	// removes all elements and frees any heap storage
	void clear()
	{
		if (isOnHeap())
			::operator delete(m_storage.pHeap);
		m_uSize = 0;
		m_uCapacity = N;
	}

private:
	bool isOnHeap() const { return m_uCapacity > N; }
	T* data() { return isOnHeap() ? m_storage.pHeap : reinterpret_cast<T*>(m_storage.auInline); }
	const T* data() const { return isOnHeap() ? m_storage.pHeap : reinterpret_cast<const T*>(m_storage.auInline); }

	void grow(size_type uNewCapacity)
	{
		T* pNew = static_cast<T*>(::operator new(uNewCapacity * sizeof(T)));
		T* pOld = data();
		for (size_type i = 0; i < m_uSize; i++)
			new (pNew + i) T(pOld[i]);
		if (isOnHeap())
			::operator delete(m_storage.pHeap);
		m_storage.pHeap = pNew;
		m_uCapacity = uNewCapacity;
	}

	void append(const CvInlineVector& other)
	{
		for (const_iterator it = other.begin(); it != other.end(); ++it)
			push_back(*it);
	}

	size_type m_uSize;
	size_type m_uCapacity; // N while the elements are stored inline
	union
	{
		T* pHeap;
		unsigned int auInline[(N * sizeof(T) + sizeof(unsigned int) - 1) / sizeof(unsigned int)];
	} m_storage;
};

// Stores all possible damage sources on a tile (terrain, improvements, cities, units)
typedef CvInlineVector<std::pair<PlayerTypes,int>, 1> DangerUnitVector;
typedef CvInlineVector<std::pair<PlayerTypes,int>, 1> DangerCityVector;
typedef std::set<std::pair<PlayerTypes,int>> UnitSet;

//todo: return more information on top of the total danger
//might help to make better decisions in tactical AI
//alternatively: try to check whether attacks are mutually exclusive b/c of units blocking each other
//unfortunately MOD_COMBATAI_TWO_PASS_DANGER doesn't really work well if blocking units move ...
//wild idea: run a reverse tactsim for the enemy first and use that to find our most endangered units
struct UnitDangerEstimate
{
	int iTotalDamage;
	int iMeleeAttacks;
	int iRangedAttacks;
	int iWorstSingleAttack;
};

struct CvDangerPlotContents
{
	CvDangerPlotContents()
	{
		m_pPlot = NULL;
		reset();
	}

	void reset()
	{
		m_bFlatPlotDamage = false;
		m_iImprovementDamage = 0;
		m_iFogCount = 0;

		//short lists live inside the plot entry, longer ones release their heap storage here
		m_apUnits.clear();
		m_apCities.clear();
		m_apCaptureUnits.clear();
	};

	int GetDanger(const CvUnit* pUnit, const SUnitIDValueContainer& extraUnitDamage, int iExtraDamage, AirActionType iAirAction);
	int GetDanger(const CvCity* pCity, const CvUnit* pPretendGarrison, const SUnitIDValueContainer& extraUnitDamage, int iExtraSelfDamage = 0);
	std::vector<CvUnit*> GetPossibleAttackers(TeamTypes eTeamForVisibilityCheck) const;

	// should not normally be used if a city or unit is known, primarily for compatibility
	int GetDanger(bool bFixedDamageOnly);

	// just for internal use
	int GetAirUnitDamage(const CvUnit* pUnit, AirActionType iAirAction = AIR_ACTION_ATTACK);

	CvPlot* m_pPlot;
	bool m_bFlatPlotDamage;
	int m_iImprovementDamage; //only one citadel can affect a unit at a time
	DangerUnitVector m_apUnits;
	DangerCityVector m_apCities;
	DangerUnitVector m_apCaptureUnits; //for civilians
	int m_iFogCount;
};

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//  CLASS:      CvDangerPlots
//!  \brief		Used to calculate the relative danger of a given plot for a player
//
//!  Key Attributes:
//!  - Replaces the AI_getPlotDanger function in CvPlayerAI
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
class CvDangerPlots
{
public:
	CvDangerPlots(void);
	~CvDangerPlots(void);

	void Init(PlayerTypes ePlayer);
	void Uninit();

	void UpdateDanger();
	int GetDanger(const CvPlot& pPlot, const CvUnit* pUnit, const SUnitIDValueContainer& unitDamageDealt, int iExtraDamage = 0, AirActionType iAirAction = AIR_ACTION_ATTACK);
	int GetDanger(const CvCity* pCity, const CvUnit* pPretendGarrison = NULL, const SUnitIDValueContainer& unitDamageDealt = SUnitIDValueContainer());
	int GetDanger(const CvPlot& pPlot, bool bFixedDamageOnly);

	std::vector<CvUnit*> GetPossibleAttackers(const CvPlot& Plot, TeamTypes eTeamForVisibilityCheck) const;
	bool IsKnownAttacker(const CvUnit* pUnit) const;
	bool AddKnownAttacker(const CvUnit* pUnit);
	const UnitSet& GetVanishedUnits() const { return m_vanishedUnits; }

	void SetDirty();
	bool IsDirty() const { return m_bDirty; }

	template<typename DangerPlots, typename Visitor>
	static void Serialize(DangerPlots& dangerPlots, Visitor& visitor);
	void Read(FDataStream& kStream);
	void Write(FDataStream& kStream) const;

protected:

	void AddFogDanger(CvPlot* pOrigin, TeamTypes eEnemyTeam, int iRange, bool bCheckOwnership);
	void UpdateDangerInternal(const PlotIndexContainer& plotsToIgnoreForZOC, bool bTurnChange, bool bWarChange);
	bool UpdateDangerSingleUnit(const CvUnit* pUnit, bool bIgnoreVisibility, const PlotIndexContainer& plotsToIgnoreForZOC);

	bool ShouldIgnorePlayer(PlayerTypes ePlayer);
	bool ShouldIgnoreUnit(const CvUnit* pUnit, bool bIgnoreVisibility = false);
	bool ShouldIgnoreCity(const CvCity* pCity, bool bIgnoreVisibility = false);
	bool ShouldIgnoreCitadel(CvPlot* pCitadelPlot, bool bIgnoreVisibility = false);

	void AssignUnitDangerValue(const CvUnit* pUnit, CvPlot* pPlot);
	void AssignCityDangerValue(const CvCity* pCity, CvPlot* pPlot);

	PlayerTypes m_ePlayer;
	bool m_bDirty;
	int m_iTurnBuilt;
	vector<CvDangerPlotContents> m_DangerPlots; //not serialized!
	UnitSet m_knownUnits;
	UnitSet m_vanishedUnits;
};

FDataStream& operator>>(FDataStream&, CvDangerPlots&);
FDataStream& operator<<(FDataStream&, const CvDangerPlots&);

#endif //CIV5_PROJECT_CLASSES_H