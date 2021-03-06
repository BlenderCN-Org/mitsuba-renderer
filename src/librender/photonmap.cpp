/*
    This file is part of Mitsuba, a physically based rendering system.

    Copyright (c) 2007-2011 by Wenzel Jakob and others.

    Mitsuba is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License Version 3
    as published by the Free Software Foundation.

    Mitsuba is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include <mitsuba/render/photonmap.h>
#include <mitsuba/render/scene.h>
#include <mitsuba/render/phase.h>
#include <fstream>

MTS_NAMESPACE_BEGIN

PhotonMap::PhotonMap(size_t maxPhotons) 
 : m_photonCount(0), m_maxPhotons(maxPhotons), m_balanced(false), m_scale(1.0f) {
	Assert(Photon::m_precompTableReady);

	/* For convenient heap addressing, the the photon list
	   entries start with number 1 */
	m_photons = (Photon *) allocAligned(sizeof(Photon) * (maxPhotons+1));
}
	
PhotonMap::PhotonMap(Stream *stream, InstanceManager *manager) { 
	m_aabb = AABB(stream);
	m_balanced = stream->readBool();
	m_maxPhotons = stream->readSize();
	m_lastInnerNode = stream->readSize();
	m_lastRChildNode = stream->readSize();
	m_scale = (Float) stream->readFloat();
	m_photonCount = stream->readSize();
	m_photons = new Photon[m_maxPhotons + 1];
	for (size_t i=1; i<=m_maxPhotons; ++i) 
		m_photons[i] = Photon(stream);
}

PhotonMap::~PhotonMap() {
	freeAligned(m_photons);
}

std::string PhotonMap::toString() const {
	std::ostringstream oss;
	oss << "PhotonMap[" << endl
		<< "  aabb = " << m_aabb.toString() << "," << endl
		<< "  photonCount = " << m_photonCount << "," << endl
		<< "  maxPhotons = " << m_maxPhotons << "," << endl
		<< "  balanced = " << m_balanced << "," << endl
		<< "  scale = " << m_scale << endl
		<< "]";
	return oss.str();
}

void PhotonMap::serialize(Stream *stream, InstanceManager *manager) const {
	Log(EDebug, "Serializing a photon map (%.2f KB)", 
		m_photonCount * 20.0f / 1024.0f);
	m_aabb.serialize(stream);
	stream->writeBool(m_balanced);
	stream->writeSize(m_maxPhotons);
	stream->writeSize(m_lastInnerNode);
	stream->writeSize(m_lastRChildNode);
	stream->writeFloat(m_scale);
	stream->writeSize(m_photonCount);
	for (size_t i=1; i<=m_maxPhotons; ++i)
		m_photons[i].serialize(stream);
}

bool PhotonMap::storePhoton(const Point &pos, const Normal &normal,
							const Vector &dir, const Spectrum &power, uint16_t depth) {
	Assert(!m_balanced);

	/* Overflow check */
	if (m_photonCount >= m_maxPhotons)
		return false;

	/* Keep track of the volume covered by all stored photons */
	m_aabb.expandBy(pos);

	m_photons[++m_photonCount] = Photon(pos, normal, dir, power, depth);

	return true;
}

bool PhotonMap::storePhoton(const Photon &photon) {
	Assert(!m_balanced);

	/* Overflow check */
	if (m_photonCount >= m_maxPhotons)
		return false;

	/* Keep track of the volume covered by all stored photons */
	m_aabb.expandBy(Point(photon.pos[0], photon.pos[1], photon.pos[2]));
	m_photons[++m_photonCount] = photon;

	return true;
}

/**
 * Relaxed partitioning algorithm based on code in stl_algo.h and Jensen's
 * reference implementation. This is *much* faster than std::partition
 * by relaxing the partitioning rules, making use of random access iterators 
 * and assuming that there is a guard element following the right boundary. 
 *
 * This function accepts *two* predicates and assumes that 
 * pred1(x) == !pred2(x) for all given elements except for a set S, 
 * where both predicates return false. After the algorithm finishes,
 * elements in S may have been placed on either side. However, all elements
 * for which pred1(x) is true are guaranteed to be on the left side, and
 * all elements for which pred2(x) is true will have been moved to the
 * right side.
 * 
 * Having two predicates is important for an efficient photon map 
 * balancing implementation. If pred2 is simply the opposite of pred1,
 * the balancing routine will slow down to a crawl when it is applied to
 * an array containing lots of elements with the same value as the pivot.
 * (In the context of photon mapping, this can for example happen when the 
 * scene contains an axis-aligned surface). Since it does not matter on 
 * which side of the split these elements end up, the relaxed method 
 * enables a much better separation into two sets.
 */
template<typename _RandomAccessIterator, 
	typename _Predicate1, typename _Predicate2>
	inline _RandomAccessIterator guarded_partition(
		_RandomAccessIterator start, _RandomAccessIterator end, 
		_Predicate1 pred1, _Predicate2 pred2) {
	end--;
	while (true) {
		while (pred1(*start)) /* Guarded */
			start++;
		while (pred2(*end) && end > start)
			end--;
		if (start >= end)
			break;
		std::iter_swap(start++, end--);
	}
	return start;
}

/**
 * This algorithm works similarly to QUICKSORT, although it does not sort
 * the array. Instead, it creates a partition such that there is an ordering 
 * of all entries with respect to the value at 'pivotIndex'. It does this by
 * repetetively partitioning according to some arbitrarily chosen pivot 
 * (currently the rightmost one) and then recursing either into the left or 
 * right halves.
 */
void PhotonMap::quickPartition(photon_iterator left, photon_iterator right, 
	photon_iterator pivot, int axis) const {
	right--;

	while (right > left) {
		const float pivotValue = (*right)->pos[axis];

		/* Relaxed quicksort-style partitioning routine. (See above
		   for an explanation). */
		photon_iterator mid = guarded_partition(left, right,
			comparePhotonLess(axis, pivotValue),
			comparePhotonGreater(axis, pivotValue)
		);

		/* Move the pivot in between the two sets */
		std::swap(*mid, *right);

		/* Is the requested pivot index located inside the left half? */
		if (mid > pivot)
			right = mid - 1;
		/* Is the requested pivot index located inside the right half? */
		else if (mid < pivot)
			left = mid + 1;
		else
			return;
	}
}

/**
 * Given a number of entries, this method calculates the maximum amount of
 * nodes on the left subtree of a left-balanced tree. There are two main 
 * cases here:
 * 
 * 1) It is possible to completely fill the left subtree
 * 2) It doesn't work - the last level contains too few nodes, e.g :
 *         O
 *        / \
 *       O   O
 *      /
 *     O
 * 
 * The function assumes that "count" > 1.
 */
size_t PhotonMap::leftSubtreeSize(size_t treeSize) const {
	/* Layer 0 contains one node */
	size_t p = 1;

	/* Traverse downwards until the first incompletely
	   filled tree level is encountered */
	while (2*p <= treeSize)
		p *= 2;

	/* Calculate the number of filled slots in the last level */
	size_t remaining = treeSize - p + 1;

	if (2*remaining < p) {
		/* Case 2: The last level contains too few nodes. Remove
		   overestimate from the left subtree node count and add
		   the remaining nodes */
		p = (p >> 1) + remaining;
	}

	return p - 1;
}

void PhotonMap::balance() {
	if (m_photonCount == 0) {
		Log(EInfo, "Photon map: no need for balancing, no photons available.");
		m_balanced = true;
		return;
	}
	Assert(!m_balanced);

	/* Shuffle pointers instead of copying photons back and forth */
	std::vector<photon_ptr> photonPointers(m_photonCount + 1);
	/* Destination for the final heap permutation. Indexed starting at 1 */
	std::vector<size_t> heapPermutation(m_photonCount + 1);

	heapPermutation[0] = 0;
	for (size_t i=0; i<=m_photonCount; i++)
		photonPointers[i] = &m_photons[i];

	ref<Timer> timer = new Timer();

	Log(EInfo, "Photon map: balancing %i photons (%s)..", m_photonCount,
		memString(sizeof(Photon) * (m_photonCount+1)).c_str());

	balanceRecursive(photonPointers.begin(), photonPointers.begin()+1, 
		photonPointers.end(), heapPermutation, m_aabb, 1);

	Log(EInfo, "Done (took %i ms)", timer->getMilliseconds());
	timer->reset();

	/* 'heapPointers' now contains a permutation representing
	    the properly left-balanced photon map. Apply this permutation
		to the photon array. */
	permute_inplace(m_photons, heapPermutation);

	Log(EInfo, "Applied permutation (took %i ms)", timer->getMilliseconds());

	/* We want to quickly be able to determine whether a node at
	   a given index is an inner node (e.g. it has left or right
	   children) and specifically, if it has a right child */
	m_lastInnerNode = m_photonCount/2;
	m_lastRChildNode = (m_photonCount-1)/2;
	m_balanced = true;
}

void PhotonMap::balanceRecursive(photon_iterator basePtr, 
	photon_iterator sortStart,
	photon_iterator sortEnd, 
	std::vector<size_t> &heapPermutation,
	AABB &aabb, size_t heapIndex) const {

	/* A fully left-balanced binary tree has this many nodes on its
	   left subtree */
	size_t leftSize = leftSubtreeSize(sortEnd - sortStart);

	/* We choose a pivot index such that the resulting tree satisfies
       this property */
	photon_iterator pivot = sortStart + leftSize;

	/* Splitting along the axis with the widest spread
       works well in practice and is cheap to compute (p.71) */
	int splitAxis = aabb.getLargestAxis();

	/* QUICKSORT-like partitioning iterations until the entry referenced by 
	   'pivot' imposes an ordering wrt. all other photons in the range */
	quickPartition(sortStart, sortEnd, pivot, splitAxis);
	Float splitPos = (*pivot)->pos[splitAxis];

	/* Update the heap permutation and record the splitting axis */
	heapPermutation[heapIndex] = *pivot - *basePtr;
	(*pivot)->axis = splitAxis;

	if (pivot > sortStart) {
		if (pivot > sortStart + 1) {
			/* There are more then two elements on the left
			   subtree. Balance them recursively */
			std::swap(aabb.max[splitAxis], splitPos);
			balanceRecursive(basePtr, sortStart, pivot, heapPermutation,
				aabb, leftChild(heapIndex));
			std::swap(aabb.max[splitAxis], splitPos);
		} else {
			/* Leaf node - just copy. */
			heapPermutation[leftChild(heapIndex)] = *sortStart - *basePtr;
		}
	}

	if (pivot < sortEnd - 1) {
		if (pivot < sortEnd - 2) {
			/* There are more then two elements on the right
			   subtree. Balance them recursively */
			std::swap(aabb.min[splitAxis], splitPos);
			balanceRecursive(basePtr, pivot+1, sortEnd, heapPermutation, 
				aabb, rightChild(heapIndex));
			std::swap(aabb.min[splitAxis], splitPos);
		} else {
			/* Leaf node - just copy. */
			heapPermutation[rightChild(heapIndex)] = *(sortEnd-1) - *basePtr;
		}
	}
}

size_t PhotonMap::nnSearch(const Point &p, Float &searchRadiusSquared, 
		size_t maxSize, search_result *results) const {
	const float pos[3] = { (float) p.x, (float) p.y, (float) p.z };
	size_t stack[MAX_PHOTONMAP_DEPTH];
	size_t index = 1, stackPos = 1, fill = 0;
	bool isPriorityQueue = false;
	float distSquared = (float) searchRadiusSquared;
	stack[0] = 0;

	while (index > 0) {
		const_photon_ptr photon = &m_photons[index];

		/* Recurse on inner nodes */
		if (isInnerNode(index)) {
			float distToPlane = pos[photon->axis] - photon->pos[photon->axis];

			/* Does the search region overlap with both split half-spaces? */
			bool searchBoth = (distToPlane*distToPlane <= distSquared);

			if (distToPlane > 0) {
				/* The search query is located on the right side of the split. 
				   Search this side first. */
				if (hasRightChild(index)) {
					if (searchBoth)
						stack[stackPos++] = leftChild(index);
					index = rightChild(index);
				} else if (searchBoth) {
					index = leftChild(index);
				} else {
					index = stack[--stackPos];
				}
			} else {
				/* The search query is located on the left side of the split. 
				   Search this side first. */
				if (searchBoth && hasRightChild(index))
					stack[stackPos++] = rightChild(index);

				index = leftChild(index);
			}
		} else {
			index = stack[--stackPos];
		}

		/* Check if the current photon is within the query's search radius */
		const float photonDistSquared = photon->distSquared(pos);

		if (photonDistSquared < distSquared) {
			/* Similarly to Jensen's implementation, the search switches 
			   to a priority queue when the available search result space 
			   is exhausted */
			if (fill < maxSize) {
				/* There is still room, just add the photon to the search 
				   result list */
				results[fill++] = search_result(photonDistSquared, photon);
			} else {
				search_result *begin = results,
							  *end = begin + maxSize + 1;
				if (!isPriorityQueue) {
					/* Establish the MAX-heap property */
					std::make_heap(begin, begin + maxSize, distanceMetric());
					isPriorityQueue = true;
				}

				/* Add the new photon, remove the one farthest away */
				results[fill] = search_result(photonDistSquared, photon);
				std::push_heap(begin, end, distanceMetric());
				std::pop_heap(begin, end, distanceMetric());

				/* Reduce the search radius accordingly */
				distSquared = results[0].first;
			}
		}
	}

	searchRadiusSquared = (Float) distSquared;
	return fill;
}

Spectrum PhotonMap::estimateIrradiance(const Point &p, const Normal &n, 
	Float searchRadius, size_t maxPhotons) const {
	Spectrum result(0.0f);

	/* The photon map needs to be balanced before executing searches */
	Assert(m_balanced);

	/* Search for photons contained within a spherical region */
	Float distSquared = searchRadius*searchRadius;
	search_result *results = static_cast<search_result *>(alloca((maxPhotons+1) 
		* sizeof(search_result)));
	size_t resultCount = nnSearch(p, distSquared, maxPhotons, results);

	/* Sum over all contributions */
	for (size_t i=0; i<resultCount; i++) {
		const Photon &photon = *results[i].second;

		/* Don't use samples from the opposite side
		   of a thin surface */
		if (dot(photon.getDirection(), n) < 0) {
			result += photon.getPower(); 
		}
	}

	/* Based on the assumption that the surface is locally flat,
	  the estimate is divided by the area of a disc corresponding to
	  the projected spherical search region */
	return result * (m_scale * INV_PI / distSquared);
}

#if !defined(MTS_SSE)
Spectrum PhotonMap::estimateIrradianceFiltered(const Point &p, const Normal &n, 
	Float searchRadius, size_t maxPhotons) const {
	Spectrum result(0.0f);

	/* The photon map needs to be balanced before executing searches */
	Assert(m_balanced);

	Float distSquared = searchRadius*searchRadius;
	search_result *results = static_cast<search_result *>(alloca((maxPhotons+1) * sizeof(search_result)));
	size_t resultCount = nnSearch(p, distSquared, maxPhotons, results);

	/* Sum over all contributions */
	for (size_t i=0; i<resultCount; i++) {
		const Float photonDistanceSqr = results[i].first;
		const Photon &photon = *results[i].second;

		/* Don't use samples from the opposite side
		   of a thin surface */
		if (dot(photon.getDirection(), n) < 0) {
			/* Weight the samples using Simpson's kernel */
			Float sqrTerm = 1.0f - photonDistanceSqr/distSquared;
			result += photon.getPower() * (sqrTerm*sqrTerm);
		}
	}

	/* Based on the assumption that the surface is locally flat,
	  the estimate is divided by the area of a disc corresponding to
	  the projected spherical search region */
	return result * (m_scale * 3 * INV_PI / distSquared);
}

#else

Spectrum PhotonMap::estimateIrradianceFiltered(const Point &p, const Normal &n, 
	Float searchRadius, size_t maxPhotons) const {
	SSEVector result(_mm_setzero_ps());

	/* The photon map needs to be balanced before executing searches */
	Assert(m_balanced);

	Float distSquared = searchRadius*searchRadius;
	search_result *results = static_cast<search_result *>(alloca((maxPhotons+1) * sizeof(search_result)));
	size_t resultCount = nnSearch(p, distSquared, maxPhotons, results);

	/* Sum over all contributions */
	for (size_t i=0; i<resultCount; i++) {
		const Float photonDistanceSqr = results[i].first;
		const Photon &photon = *results[i].second;

		if (dot(photon.getDirection(), n) > 0)
			continue;

		const Float sqrTerm = 1.0f - photonDistanceSqr/distSquared,
			 weight = sqrTerm*sqrTerm;

		const __m128 
			mantissa = _mm_cvtepi32_ps(
				_mm_set_epi32(photon.power[0], photon.power[1], photon.power[2], 0)),
			exponent = _mm_load1_ps(&Photon::m_expTable[photon.power[3]]),
			packedWeight = _mm_load1_ps(&weight),
			value = _mm_mul_ps(packedWeight, _mm_mul_ps(mantissa, exponent));

		result.ps = _mm_add_ps(value, result.ps);
	}

	/* Based on the assumption that the surface is locally flat,
	  the estimate is divided by the area of a disc corresponding to
	  the projected spherical search region */
	result.ps = _mm_mul_ps(result.ps, _mm_set1_ps(m_scale * 3 * INV_PI / distSquared));

	Spectrum spec;
	spec.fromLinearRGB(result.f[3], result.f[2], result.f[1]);

	return spec;
}
#endif

Spectrum PhotonMap::estimateRadianceFiltered(const Intersection &its,
	Float searchRadius, size_t maxPhotons) const {
	Spectrum result(0.0f);
	const BSDF *bsdf = its.shape->getBSDF();

	/* The photon map needs to be balanced before executing searches */
	Assert(m_balanced);

	/* Search for photons contained within a spherical region */
	Float distSquared = searchRadius*searchRadius;
	search_result *results = static_cast<search_result *>(alloca((maxPhotons+1) * sizeof(search_result)));
	size_t resultCount = nnSearch(its.p, distSquared, maxPhotons, results);

	/* Sum over all contributions */
	for (size_t i=0; i<resultCount; i++) {
		const Float photonDistanceSqr = results[i].first;
		const Photon &photon = *results[i].second;

		Vector wi = its.toLocal(-photon.getDirection());

		const Float sqrTerm = 1.0f - photonDistanceSqr/distSquared,
			weight = sqrTerm*sqrTerm;

		result += photon.getPower() * (bsdf->f(BSDFQueryRecord(its, wi)) * weight);
	}
	/* Based on the assumption that the surface is locally flat,
	  the estimate is divided by the area of a disc corresponding to
	  the projected spherical search region */
	return result * (m_scale * 3 * INV_PI / distSquared);
}

size_t PhotonMap::estimateRadianceRaw(const Intersection &its,
	Float searchRadius, Spectrum &result, int maxDepth) const {
	result = Spectrum(0.0f);
	const BSDF *bsdf = its.shape->getBSDF();
	
	/* The photon map needs to be balanced before executing searches */
	Assert(m_balanced);

	const float pos[3] = { (float) its.p.x, (float) its.p.y, (float) its.p.z };
	size_t stack[MAX_PHOTONMAP_DEPTH];
	size_t index = 1, stackPos = 1, resultCount = 0;
	float distSquared = (float) searchRadius*searchRadius;
	stack[0] = 0;

	while (index > 0) {
		const_photon_ptr photon = &m_photons[index];

		/* Recurse on inner nodes */
		if (isInnerNode(index)) {
			float distToPlane = pos[photon->axis] - photon->pos[photon->axis];

			/* Does the search region overlap with both split half-spaces? */
			bool searchBoth = (distToPlane*distToPlane <= distSquared);

			if (distToPlane > 0) {
				/* The search query is located on the right side of the split. 
				   Search this side first. */
				if (hasRightChild(index)) {
					if (searchBoth)
						stack[stackPos++] = leftChild(index);
					index = rightChild(index);
				} else if (searchBoth) {
					index = leftChild(index);
				} else {
					index = stack[--stackPos];
				}
			} else {
				/* The search query is located on the left side of the split. 
				   Search this side first. */
				if (searchBoth && hasRightChild(index))
					stack[stackPos++] = rightChild(index);

				index = leftChild(index);
			}
		} else {
			index = stack[--stackPos];
		}

		/* Check if the current photon is within the query's search radius */
		const float photonDistSquared = photon->distSquared(pos);

		if (photonDistSquared < distSquared) {
			Normal photonNormal(photon->getNormal());
			Vector wiWorld = -photon->getDirection();
			if (photon->getDepth() > maxDepth || dot(photonNormal, its.shFrame.n) < .1 
				|| dot(photonNormal, wiWorld) < 1e-2)
				continue;

			Vector wiLocal = its.toLocal(wiWorld);

			BSDFQueryRecord bRec(its, wiLocal);
			bRec.quantity = EImportance;
			std::swap(bRec.wi, bRec.wo);

			Spectrum weight(1.0f);

			/* Account for non-symmetry due to shading normals */
			result += photon->getPower() * bsdf->f(bRec) * 
				std::abs(Frame::cosTheta(wiLocal) / dot(photonNormal, wiWorld));

			resultCount++;
		}
	}

	return resultCount;
}

Spectrum PhotonMap::estimateVolumeRadiance(const MediumSamplingRecord &mRec, const Ray &ray,
	Float searchRadius, size_t maxPhotons, const Medium *medium) const {
	Spectrum result(0.0f);

	/* The photon map needs to be balanced before executing searches */
	Assert(m_balanced);

	/* Search for photons contained within a spherical region */
	Float distSquared = searchRadius*searchRadius;
	search_result *results = static_cast<search_result *>(alloca((maxPhotons+1) * sizeof(search_result)));
	size_t resultCount = nnSearch(ray.o, distSquared, maxPhotons, results);

	const PhaseFunction *phase = medium->getPhaseFunction();
	Vector wo = -ray.d;

	/* Sum over all contributions */
	for (size_t i=0; i<resultCount; i++) {
		const Photon &photon = *results[i].second;
		result += photon.getPower() * (phase->f(
			PhaseFunctionQueryRecord(mRec, photon.getDirection(), wo)));
	}

	const Float volFactor = (4/(Float) 3) * (Float) M_PI * 
		distSquared * std::sqrt(distSquared);
	return result * (m_scale / volFactor);
}

void PhotonMap::dumpOBJ(const std::string &filename) {
	std::ofstream os(filename.c_str());
	os << "o Photons" << endl;
	for (size_t i=1; i<=getPhotonCount(); i++) {
		Point p = getPhoton(i).getPosition();
		os << "v " << p.x << " " << p.y << " " << p.z << endl;
	}

	/// Need to generate some fake geometry so that blender will import the points
	for (size_t i=3; i<=getPhotonCount(); i++) 
		os << "f " << i << " " << i-1 << " " << i-2 << endl;
	os.close();
}

MTS_IMPLEMENT_CLASS_S(PhotonMap, false, Object)
MTS_NAMESPACE_END
