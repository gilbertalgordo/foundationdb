#include "fdbclient/SpecialKeySpace.actor.h"
#include "flow/UnitTest.h"
#include "flow/actorcompiler.h" // This must be the last #include.

// This function will normalize the given KeySelector to a standard KeySelector:
// orEqual == false && offset == 1 (Standard form)
// If the corresponding key is not in this special key range, it will move as far as possible to adjust the offset to 1
// It does have overhead here since we query all keys twice in the worst case.
// However, moving the KeySelector while handling other parameters like limits makes the code much more complex and hard
// to maintain Separate each part to make the code easy to understand and more compact
ACTOR Future<Void> SpecialKeyRangeBaseImpl::normalizeKeySelectorActor(const SpecialKeyRangeBaseImpl* pkrImpl,
                                                                      Reference<ReadYourWritesTransaction> ryw,
                                                                      KeySelector* ks) {
	ASSERT(!ks->orEqual); // should be removed before calling
	ASSERT(ks->offset != 1); // never being called if KeySelector is already normalized

	state Key startKey(pkrImpl->range.begin);
	state Key endKey(pkrImpl->range.end);

	if (ks->offset < 1) {
		// less than the given key
		if (pkrImpl->range.contains(ks->getKey())) endKey = keyAfter(ks->getKey());
	} else {
		// greater than the given key
		if (pkrImpl->range.contains(ks->getKey())) startKey = ks->getKey();
	}

	TraceEvent("NormalizeKeySelector")
	    .detail("OriginalKey", ks->getKey())
	    .detail("OriginalOffset", ks->offset)
	    .detail("SpecialKeyRangeStart", pkrImpl->range.begin)
	    .detail("SpecialKeyRangeEnd", pkrImpl->range.end);

	Standalone<RangeResultRef> result = wait(pkrImpl->getRange(ryw, KeyRangeRef(startKey, endKey)));
	if (result.size() == 0) {
		TraceEvent("ZeroElementsIntheRange").detail("Start", startKey).detail("End", endKey);
		return Void();
	}
	// TODO : KeySelector::setKey has byte limit according to the knobs, customize it if needed
	if (ks->offset < 1) {
		if (result.size() >= 1 - ks->offset) {
			ks->setKey(KeyRef(ks->arena(), result[result.size() - (1 - ks->offset)].key));
			ks->offset = 1;
		} else {
			ks->setKey(KeyRef(ks->arena(), result[0].key));
			ks->offset += result.size();
		}
	} else {
		if (result.size() >= ks->offset) {
			ks->setKey(KeyRef(ks->arena(), result[ks->offset - 1].key));
			ks->offset = 1;
		} else {
			ks->setKey(KeyRef(ks->arena(), keyAfter(result[result.size() - 1].key)));
			ks->offset -= result.size();
		}
	}
	TraceEvent("NormalizeKeySelector")
	    .detail("NormalizedKey", ks->getKey())
	    .detail("NormalizedOffset", ks->offset)
	    .detail("SpecialKeyRangeStart", pkrImpl->range.begin)
	    .detail("SpecialKeyRangeEnd", pkrImpl->range.end);
	return Void();
}

ACTOR Future<Standalone<RangeResultRef>> SpecialKeySpace::getRangeAggregationActor(
    SpecialKeySpace* pks, Reference<ReadYourWritesTransaction> ryw, KeySelector begin, KeySelector end,
    GetRangeLimits limits, bool reverse) {
	// This function handles ranges which cover more than one keyrange and aggregates all results
	// KeySelector, GetRangeLimits and reverse are all handled here
	state Standalone<RangeResultRef> result;
	state RangeMap<Key, SpecialKeyRangeBaseImpl*, KeyRangeRef>::Iterator iter;
	state int actualBeginOffset;
	state int actualEndOffset;
	// make sure orEqual == false
	begin.removeOrEqual(begin.arena());
	end.removeOrEqual(end.arena());

	// make sure offset == 1
	state RangeMap<Key, SpecialKeyRangeBaseImpl*, KeyRangeRef>::Iterator beginIter =
	    pks->impls.rangeContaining(begin.getKey());
	while ((begin.offset < 1 && beginIter != pks->impls.ranges().begin()) ||
	       (begin.offset > 1 && beginIter != pks->impls.ranges().end())) {
		if (beginIter->value() != nullptr)
			wait(beginIter->value()->normalizeKeySelectorActor(beginIter->value(), ryw, &begin));
		begin.offset < 1 ? --beginIter : ++beginIter;
	}

	actualBeginOffset = begin.offset;
	if (beginIter == pks->impls.ranges().begin())
		begin.setKey(normalKeys.begin);
	else if (beginIter == pks->impls.ranges().end())
		begin.setKey(normalKeys.end);

	if (!begin.isFirstGreaterOrEqual()) {
		// The Key Selector points to key outside the whole special key space
		TraceEvent(SevInfo, "BeginKeySelectorPointsOutside")
		    .detail("TerminateKey", begin.getKey())
		    .detail("TerminateOffset", begin.offset);
		if (begin.offset < 1 && beginIter == pks->impls.ranges().begin())
			result.readToBegin = true;
		else
			result.readThroughEnd = true;
		begin.offset = 1;
	}
	state RangeMap<Key, SpecialKeyRangeBaseImpl*, KeyRangeRef>::Iterator endIter =
	    pks->impls.rangeContaining(end.getKey());
	while ((end.offset < 1 && endIter != pks->impls.ranges().begin()) ||
	       (end.offset > 1 && endIter != pks->impls.ranges().end())) {
		if (endIter->value() != nullptr) wait(endIter->value()->normalizeKeySelectorActor(endIter->value(), ryw, &end));
		end.offset < 1 ? --endIter : ++endIter;
	}

	actualEndOffset = end.offset;
	if (endIter == pks->impls.ranges().begin())
		end.setKey(normalKeys.begin);
	else if (endIter == pks->impls.ranges().end())
		end.setKey(normalKeys.end);

	if (!end.isFirstGreaterOrEqual()) {
		// The Key Selector points to key outside the whole special key space
		TraceEvent(SevInfo, "EndKeySelectorPointsOutside")
		    .detail("TerminateKey", end.getKey())
		    .detail("TerminateOffset", end.offset);
		if (end.offset < 1 && endIter == pks->impls.ranges().begin())
			result.readToBegin = true;
		else
			result.readThroughEnd = true;
		end.offset = 1;
	}
	// Handle all corner cases like what RYW does
	// return if range inverted
	if (actualBeginOffset >= actualEndOffset && begin.getKey() >= end.getKey()) {
		TEST(true);
		return RangeResultRef(false, false);
	}
	// If touches begin or end, return with readToBegin and readThroughEnd flags
	if (beginIter == pks->impls.ranges().end() || endIter == pks->impls.ranges().begin()) {
		TEST(true);
		return result;
	}
	state RangeMap<Key, SpecialKeyRangeBaseImpl*, KeyRangeRef>::Ranges ranges =
	    pks->impls.intersectingRanges(KeyRangeRef(begin.getKey(), end.getKey()));
	// TODO : workaround to write this two together to make the code compact
	// The issue here is boost::iterator_range<> doest not provide rbegin(), rend()
	iter = reverse ? ranges.end() : ranges.begin();
	if (reverse) {
		while (iter != ranges.begin()) {
			--iter;
			if (iter->value() == nullptr) continue;
			KeyRangeRef kr = iter->range();
			KeyRef keyStart = kr.contains(begin.getKey()) ? begin.getKey() : kr.begin;
			KeyRef keyEnd = kr.contains(end.getKey()) ? end.getKey() : kr.end;
			Standalone<RangeResultRef> pairs = wait(iter->value()->getRange(ryw, KeyRangeRef(keyStart, keyEnd)));
			// limits handler
			for (int i = pairs.size() - 1; i >= 0; --i) {
				result.push_back_deep(result.arena(), pairs[i]);
				// Note : behavior here is even the last k-v pair makes total bytes larger than specified, it is still
				// returned In other words, the total size of the returned value (less the last entry) will be less than
				// byteLimit
				limits.decrement(pairs[i]);
				if (limits.isReached()) {
					// result.more = (iter != ranges.begin()) || (iter == ranges.begin() && i != 0);
					result.more = true;
					result.readToBegin = false;
					return result;
				};
			}
		}
	} else {
		for (iter = ranges.begin(); iter != ranges.end(); ++iter) {
			if (iter->value() == nullptr) continue;
			KeyRangeRef kr = iter->range();
			KeyRef keyStart = kr.contains(begin.getKey()) ? begin.getKey() : kr.begin;
			KeyRef keyEnd = kr.contains(end.getKey()) ? end.getKey() : kr.end;
			Standalone<RangeResultRef> pairs = wait(iter->value()->getRange(ryw, KeyRangeRef(keyStart, keyEnd)));
			// limits handler
			for (int i = 0; i < pairs.size(); ++i) {
				result.push_back_deep(result.arena(), pairs[i]);
				// Note : behavior here is even the last k-v pair makes total bytes larger than specified, it is still
				// returned In other words, the total size of the returned value (less the last entry) will be less than
				// byteLimit
				limits.decrement(pairs[i]);
				if (limits.isReached()) {
					// result.more = (iter != ranges.end()) || (iter == ranges.end() && i != pairs.size() - 1);
					result.more = true;
					result.readThroughEnd = false;
					return result;
				};
			}
		}
	}
	return result;
}

Future<Standalone<RangeResultRef>> SpecialKeySpace::getRange(Reference<ReadYourWritesTransaction> ryw,
                                                             KeySelector begin, KeySelector end, GetRangeLimits limits,
                                                             bool snapshot, bool reverse) {
	// validate limits here
	if (!limits.isValid()) return range_limits_invalid();
	if (limits.isReached()) {
		TEST(true); // read limit 0
		return Standalone<RangeResultRef>();
	}
	// ignore snapshot, which is not used
	return getRangeAggregationActor(this, ryw, begin, end, limits, reverse);
}

ACTOR Future<Optional<Value>> SpecialKeySpace::getActor(SpecialKeySpace* pks, Reference<ReadYourWritesTransaction> ryw,
                                                        KeyRef key) {
	// use getRange to workaround this
	Standalone<RangeResultRef> result = wait(pks->getRange(
	    ryw, KeySelector(firstGreaterOrEqual(key)), KeySelector(firstGreaterOrEqual(keyAfter(key))), GetRangeLimits(CLIENT_KNOBS->TOO_MANY)));
	ASSERT(result.size() <= 1);
	if (result.size()) {
		return Optional<Value>(result[0].value);
	} else {
		return Optional<Value>();
	}
}

Future<Optional<Value>> SpecialKeySpace::get(Reference<ReadYourWritesTransaction> ryw, const Key& key, bool snapshot) {
	// ignore snapshot, which is not used
	return getActor(this, ryw, key);
}

class SpecialKeyRangeTestImpl : public SpecialKeyRangeBaseImpl {
public:
	explicit SpecialKeyRangeTestImpl(KeyRef start, KeyRef end, const std::string& prefix, int size)
	  : SpecialKeyRangeBaseImpl(start, end), prefix(prefix), size(size) {
		ASSERT(size > 0);
		for (int i = 0; i < size; ++i) {
			kvs.push_back_deep(kvs.arena(),
			                   KeyValueRef(getKeyForIndex(i), deterministicRandom()->randomAlphaNumeric(16)));
		}
	}

	KeyValueRef getKeyValueForIndex(int idx) { return kvs[idx]; }

	Key getKeyForIndex(int idx) { return Key(prefix + format("%010d", idx)).withPrefix(range.begin); }
	int getSize() { return size; }
	virtual Future<Standalone<RangeResultRef>> getRange(Reference<ReadYourWritesTransaction> ryw,
	                                                    KeyRangeRef kr) const override {
		int startIndex = 0, endIndex = size;
		while (startIndex < size && kvs[startIndex].key < kr.begin) ++startIndex;
		while (endIndex > startIndex && kvs[endIndex - 1].key >= kr.end) --endIndex;
		if (startIndex == endIndex)
			return Standalone<RangeResultRef>();
		else
			return Standalone<RangeResultRef>(RangeResultRef(kvs.slice(startIndex, endIndex), false));
	}

private:
	Standalone<VectorRef<KeyValueRef>> kvs;
	std::string prefix;
	int size;
};

TEST_CASE("/fdbclient/SpecialKeySpace/Unittest") {
	SpecialKeySpace pks(LiteralStringRef("\xff\xff"), LiteralStringRef("\xff\xff\xff"));
	SpecialKeyRangeTestImpl pkr1(LiteralStringRef("\xff\xff/cat/"), LiteralStringRef("\xff\xff/cat/\xff"), "small", 10);
	SpecialKeyRangeTestImpl pkr2(LiteralStringRef("\xff\xff/dog/"), LiteralStringRef("\xff\xff/dog/\xff"), "medium",
	                             100);
	SpecialKeyRangeTestImpl pkr3(LiteralStringRef("\xff\xff/pig/"), LiteralStringRef("\xff\xff/pig/\xff"), "large",
	                             1000);
	pks.registerKeyRange(pkr1.getKeyRange(), &pkr1);
	pks.registerKeyRange(pkr2.getKeyRange(), &pkr2);
	pks.registerKeyRange(pkr3.getKeyRange(), &pkr3);
	auto nullRef = Reference<ReadYourWritesTransaction>();
	// get
	{
		auto resultFuture = pks.get(nullRef, LiteralStringRef("\xff\xff/cat/small0000000009"));
		ASSERT(resultFuture.isReady());
		auto result = resultFuture.getValue().get();
		ASSERT(result == pkr1.getKeyValueForIndex(9).value);
		auto emptyFuture = pks.get(nullRef, LiteralStringRef("\xff\xff/cat/small0000000010"));
		ASSERT(emptyFuture.isReady());
		auto emptyResult = emptyFuture.getValue();
		ASSERT(!emptyResult.present());
	}
	// general getRange
	{
		KeySelector start = KeySelectorRef(LiteralStringRef("\xff\xff/elepant"), false, -9);
		KeySelector end = KeySelectorRef(LiteralStringRef("\xff\xff/frog"), false, +11);
		auto resultFuture = pks.getRange(nullRef, start, end, GetRangeLimits());
		ASSERT(resultFuture.isReady());
		auto result = resultFuture.getValue();
		ASSERT(result.size() == 20);
		ASSERT(result[0].key == pkr2.getKeyForIndex(90));
		ASSERT(result[result.size() - 1].key == pkr3.getKeyForIndex(9));
	}
	// KeySelector points outside
	{
		KeySelector start = KeySelectorRef(pkr3.getKeyForIndex(999), true, -1110);
		KeySelector end = KeySelectorRef(pkr1.getKeyForIndex(0), false, +1112);
		auto resultFuture = pks.getRange(nullRef, start, end, GetRangeLimits());
		ASSERT(resultFuture.isReady());
		auto result = resultFuture.getValue();
		ASSERT(result.size() == 1110);
		ASSERT(result[0].key == pkr1.getKeyForIndex(0));
		ASSERT(result[result.size() - 1].key == pkr3.getKeyForIndex(999));
	}
	// GetRangeLimits with row limit
	{
		KeySelector start = KeySelectorRef(pkr2.getKeyForIndex(0), true, 0);
		KeySelector end = KeySelectorRef(pkr3.getKeyForIndex(0), false, 0);
		auto resultFuture = pks.getRange(nullRef, start, end, GetRangeLimits(2));
		ASSERT(resultFuture.isReady());
		auto result = resultFuture.getValue();
		ASSERT(result.size() == 2);
		ASSERT(result[0].key == pkr2.getKeyForIndex(0));
		ASSERT(result[1].key == pkr2.getKeyForIndex(1));
	}
	// GetRangeLimits with byte limit
	{
		KeySelector start = KeySelectorRef(pkr2.getKeyForIndex(0), true, 0);
		KeySelector end = KeySelectorRef(pkr3.getKeyForIndex(0), false, 0);
		auto resultFuture = pks.getRange(nullRef, start, end, GetRangeLimits(10, 100));
		ASSERT(resultFuture.isReady());
		auto result = resultFuture.getValue();
		int bytes = 0;
		for (int i = 0; i < result.size() - 1; ++i) bytes += 8 + pkr2.getKeyValueForIndex(i).expectedSize();
		ASSERT(bytes < 100);
		ASSERT(bytes + 8 + pkr2.getKeyValueForIndex(result.size()).expectedSize() >= 100);
	}
	// reverse test with overlapping key range
	{
		KeySelector start = KeySelectorRef(pkr2.getKeyForIndex(0), true, 0);
		KeySelector end = KeySelectorRef(pkr3.getKeyForIndex(999), true, +1);
		auto resultFuture = pks.getRange(nullRef, start, end, GetRangeLimits(1100), false, true);
		ASSERT(resultFuture.isReady());
		auto result = resultFuture.getValue();
		for (int i = 0; i < pkr3.getSize(); ++i) ASSERT(result[i] == pkr3.getKeyValueForIndex(pkr3.getSize() - 1 - i));
		for (int i = 0; i < pkr2.getSize(); ++i)
			ASSERT(result[i + pkr3.getSize()] == pkr2.getKeyValueForIndex(pkr2.getSize() - 1 - i));
	}
	return Void();
}
