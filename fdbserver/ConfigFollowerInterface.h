/*
 * ConfigFollowerInterface.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "fdbclient/CommitTransaction.h"
#include "fdbclient/FDBTypes.h"
#include "fdbrpc/fdbrpc.h"
#include "fdbserver/CoordinationInterface.h"

struct ConfigFollowerGetVersionReply {
	static constexpr FileIdentifier file_identifier = 1028349;
	Version version;

	explicit ConfigFollowerGetVersionReply(Version version = -1) : version(version) {}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, version);
	}
};

struct ConfigFollowerGetVersionRequest {
	static constexpr FileIdentifier file_identifier = 9840156;
	ReplyPromise<ConfigFollowerGetVersionRequest> reply;

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, reply);
	}
};

struct ConfigFollowerGetFullDatabaseReply {
	static constexpr FileIdentifier file_identifier = 1734095;
	std::map<Key, Value> database;

	ConfigFollowerGetFullDatabaseReply() = default;
	explicit ConfigFollowerGetFullDatabaseReply(std::map<Key, Value> const& database) : database(database) {
		// TODO: Support move constructor as well
	}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, database);
	}
};

struct ConfigFollowerGetFullDatabaseRequest {
	static constexpr FileIdentifier file_identifier = 294811;
	Version version;
	Optional<Value> filter;
	ReplyPromise<ConfigFollowerGetFullDatabaseReply> reply;

	ConfigFollowerGetFullDatabaseRequest() : version(-1) {}
	explicit ConfigFollowerGetFullDatabaseRequest(Version version, Optional<Value> filter)
	  : version(version), filter(filter) {}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, version, filter, reply);
	}
};

struct ConfigFollowerGetChangesReply {
	static constexpr FileIdentifier file_identifier = 234859;
	Version mostRecentVersion;
	Standalone<VectorRef<MutationRef>> mutations;

	ConfigFollowerGetChangesReply() : mostRecentVersion(-1) {}
	explicit ConfigFollowerGetChangesReply(Version mostRecentVersion,
	                                       Standalone<VectorRef<MutationRef>> const& mutations)
	  : mostRecentVersion(mostRecentVersion), mutations(mutations) {}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, mostRecentVersion, mutations);
	}
};

struct ConfigFollowerGetChangesRequest {
	static constexpr FileIdentifier file_identifier = 178935;
	Version lastSeenVersion;
	Optional<Value> filter;
	ReplyPromise<ConfigFollowerGetChangesReply> reply;

	ConfigFollowerGetChangesRequest() : lastSeenVersion(-1) {}
	explicit ConfigFollowerGetChangesRequest(Version lastSeenVersion, Optional<Value> filter)
	  : lastSeenVersion(lastSeenVersion), filter(filter) {}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, lastSeenVersion, reply);
	}
};

struct ConfigFollowerInterface {
	static constexpr FileIdentifier file_identifier = 7721102;
	RequestStream<ConfigFollowerGetVersionRequest> getVersion;
	RequestStream<ConfigFollowerGetFullDatabaseRequest> getFullDatabase;
	RequestStream<ConfigFollowerGetChangesRequest> getChanges;

	ConfigFollowerInterface() = default;
	void setupWellKnownEndpoints() {
		getVersion.makeWellKnownEndpoint(WLTOKEN_CONFIGFOLLOWER_GETVERSION, TaskPriority::Coordination);
		getFullDatabase.makeWellKnownEndpoint(WLTOKEN_CONFIGFOLLOWER_GETFULLDB, TaskPriority::Coordination);
		getChanges.makeWellKnownEndpoint(WLTOKEN_CONFIGFOLLOWER_GETCHANGES, TaskPriority::Coordination);
	}

	ConfigFollowerInterface(NetworkAddress const& remote)
	  : getVersion(Endpoint({ remote }, WLTOKEN_CONFIGFOLLOWER_GETVERSION)),
	    getFullDatabase(Endpoint({ remote }, WLTOKEN_CONFIGFOLLOWER_GETFULLDB)),
	    getChanges(Endpoint({ remote }, WLTOKEN_CONFIGFOLLOWER_GETCHANGES)) {}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, getVersion, getFullDatabase, getChanges);
	}
};
