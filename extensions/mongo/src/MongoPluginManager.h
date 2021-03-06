/**
*** Copyright (c) 2016-present,
*** Jaguar0625, gimre, BloodyRookie, Tech Bureau, Corp. All rights reserved.
***
*** This file is part of Catapult.
***
*** Catapult is free software: you can redistribute it and/or modify
*** it under the terms of the GNU Lesser General Public License as published by
*** the Free Software Foundation, either version 3 of the License, or
*** (at your option) any later version.
***
*** Catapult is distributed in the hope that it will be useful,
*** but WITHOUT ANY WARRANTY; without even the implied warranty of
*** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*** GNU Lesser General Public License for more details.
***
*** You should have received a copy of the GNU Lesser General Public License
*** along with Catapult. If not, see <http://www.gnu.org/licenses/>.
**/

#pragma once
#include "ExternalCacheStorageBuilder.h"
#include "MongoStorageContext.h"
#include "MongoTransactionPlugin.h"
#include "catapult/model/BlockChainConfiguration.h"
#include "catapult/plugins.h"

namespace catapult { namespace mongo {

	/// A manager for registering mongo plugins.
	class MongoPluginManager {
	public:
		/// Creates a new plugin manager around \a mongoContext and \a chainConfig.
		explicit MongoPluginManager(MongoStorageContext& mongoContext, const model::BlockChainConfiguration& chainConfig)
				: m_mongoContext(mongoContext)
				, m_chainConfig(chainConfig)
		{}

	public:
		/// Gets the mongo storage context.
		const MongoStorageContext& mongoContext() const {
			return m_mongoContext;
		}

		/// Gets the block chain configuration.
		const model::BlockChainConfiguration& chainConfig() const {
			return m_chainConfig;
		}

		/// Creates a mongo database connection.
		MongoDatabase createDatabaseConnection() {
			return m_mongoContext.createDatabaseConnection();
		}

	public:
		/// Adds support for a transaction described by \a pTransactionPlugin.
		void addTransactionSupport(std::unique_ptr<MongoTransactionPlugin>&& pTransactionPlugin) {
			m_transactionRegistry.registerPlugin(std::move(pTransactionPlugin));
		}

		/// Adds support for an external cache storage described by \a pStorage.
		template<typename TStorage>
		void addStorageSupport(std::unique_ptr<TStorage>&& pStorage) {
			m_storageBuilder.add(std::move(pStorage));
		}

	public:
		/// Gets the transaction registry.
		const MongoTransactionRegistry& transactionRegistry() const {
			return m_transactionRegistry;
		}

		/// Creates an external cache storage.
		std::unique_ptr<ExternalCacheStorage> createStorage() {
			return m_storageBuilder.build();
		}

	private:
		MongoStorageContext& m_mongoContext;
		model::BlockChainConfiguration m_chainConfig;
		MongoTransactionRegistry m_transactionRegistry;
		ExternalCacheStorageBuilder m_storageBuilder;
	};
}}

/// Entry point for registering a dynamic module with \a manager.
extern "C" PLUGIN_API
void RegisterMongoSubsystem(catapult::mongo::MongoPluginManager& manager);
