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
#include "BlockChainProcessor.h"
#include "BlockChainSyncHandlers.h"
#include "HashCheckOptions.h"
#include "InputUtils.h"
#include "catapult/chain/ChainFunctions.h"
#include "catapult/disruptor/DisruptorConsumer.h"
#include "catapult/validators/ParallelValidationPolicy.h"

namespace catapult {
	namespace chain { struct CatapultState; }
	namespace io { class BlockStorageCache; }
	namespace model { class TransactionRegistry; }
	namespace utils { class TimeSpan; }
}

namespace catapult { namespace consumers {

	/// Creates a consumer that calculates hashes of all entities using \a transactionRegistry.
	disruptor::BlockConsumer CreateBlockHashCalculatorConsumer(const model::TransactionRegistry& transactionRegistry);

	/// Creates a consumer that checks entities for previous processing based on their hash.
	/// \a timeSupplier is used for generating timestamps and \a options specifies additional cache options.
	disruptor::ConstBlockConsumer CreateBlockHashCheckConsumer(const chain::TimeSupplier& timeSupplier, const HashCheckOptions& options);

	/// Creates a consumer that extracts all addresses affected by transactions using \a notificationPublisher.
	disruptor::BlockConsumer CreateBlockAddressExtractionConsumer(const model::NotificationPublisher& notificationPublisher);

	/// Creates a consumer that checks a block chain for internal integrity.
	/// A valid chain must have no more than \a maxChainSize blocks and end no more than \a maxBlockFutureTime past the current time
	/// supplied by \a timeSupplier.
	disruptor::ConstBlockConsumer CreateBlockChainCheckConsumer(
			uint32_t maxChainSize,
			const utils::TimeSpan& maxBlockFutureTime,
			const chain::TimeSupplier& timeSupplier);

	/// Predicate for checking whether or not an entity requires validation.
	using RequiresValidationPredicate = model::MatchingEntityPredicate;

	/// Creates a consumer that runs stateless validation using \a pValidator and the specified policy
	/// (\a pValidationPolicy). Validation will only be performed for entities for which \a requiresValidationPredicate
	/// returns \c true.
	disruptor::ConstBlockConsumer CreateBlockStatelessValidationConsumer(
			const std::shared_ptr<const validators::stateless::AggregateEntityValidator>& pValidator,
			const std::shared_ptr<const validators::ParallelValidationPolicy>& pValidationPolicy,
			const RequiresValidationPredicate& requiresValidationPredicate);

	/// Creates a consumer that attempts to synchronize a remote chain with the local chain, which is composed of
	/// state (in \a cache and \a state) and blocks (in \a storage).
	/// \a maxRollbackBlocks The maximum number of blocks that can be rolled back.
	/// \a handlers are used to customize the sync process.
	/// \note This consumer is non-const because it updates the element generation hashes.
	disruptor::DisruptorConsumer CreateBlockChainSyncConsumer(
			cache::CatapultCache& cache,
			state::CatapultState& state,
			io::BlockStorageCache& storage,
			uint32_t maxRollbackBlocks,
			const BlockChainSyncHandlers& handlers);

	/// Prototype for a function that is called with a new block.
	using NewBlockSink = consumer<const std::shared_ptr<const model::Block>&>;

	/// Creates a consumer that calls \a newBlockSink with new blocks that have a source in \a sinkSourceMask.
	/// \note This consumer must be last because it might destroy the input.
	disruptor::DisruptorConsumer CreateNewBlockConsumer(const NewBlockSink& newBlockSink, disruptor::InputSource sinkSourceMask);
}}
