#include "ChainSynchronizer.h"
#include "CompareChains.h"
#include "RemoteApi.h"
#include "catapult/api/ChainApi.h"
#include "catapult/model/BlockChainConfiguration.h"
#include "catapult/thread/FutureUtils.h"
#include "catapult/utils/SpinLock.h"
#include <queue>

namespace catapult { namespace chain {

	namespace {
		using NodeInteractionFuture = thread::future<NodeInteractionResult>;

		struct ElementInfo {
			disruptor::DisruptorElementId Id;
			Height EndHeight;
			size_t NumBytes;
		};

		class UnprocessedElements : public std::enable_shared_from_this<UnprocessedElements> {
		public:
			explicit UnprocessedElements(const CompletionAwareBlockRangeConsumerFunc& blockRangeConsumer, size_t maxSize)
					: m_blockRangeConsumer(blockRangeConsumer)
					, m_maxSize(maxSize)
					, m_numBytes(0)
					, m_hasPendingSync(false)
					, m_dirty(false)
			{}

		public:
			bool empty() {
				return 0 == numBytes();
			}

			size_t numBytes() {
				SpinLockGuard guard(m_spinLock);
				return m_numBytes;
			}

			bool shouldStartSync() {
				SpinLockGuard guard(m_spinLock);
				if (m_numBytes >= m_maxSize || m_hasPendingSync || m_dirty)
					return false;

				m_hasPendingSync = true;
				return true;
			}

			Height maxHeight() {
				SpinLockGuard guard(m_spinLock);
				return m_elements.empty() ? Height(0) : m_elements.back().EndHeight;
			}

			bool add(model::BlockRange&& range) {
				SpinLockGuard guard(m_spinLock);
				if (m_dirty)
					return false;

				auto endHeight = (--range.cend())->Height;
				auto bufferSize = range.totalSize();

				// need to use shared_from_this because dispatcher can finish processing a block after
				// scheduler is stopped (and owning DefaultChainSynchronizer is destroyed)
				auto newId = m_blockRangeConsumer(std::move(range), [pThis = shared_from_this()](auto id, auto result) {
					pThis->remove(id, result.CompletionStatus);
				});
				auto info = ElementInfo{ newId, endHeight, bufferSize };
				m_numBytes += info.NumBytes;
				m_elements.emplace(info);
				return true;
			}

			void remove(disruptor::DisruptorElementId id, disruptor::CompletionStatus status) {
				SpinLockGuard guard(m_spinLock);
				const auto& info = m_elements.front();
				if (info.Id != id)
					CATAPULT_THROW_INVALID_ARGUMENT_1("unexpected element id", id);

				m_numBytes -= info.NumBytes;
				m_elements.pop();
				m_dirty = hasPendingOperation() && disruptor::CompletionStatus::Normal != status;
			}

			void clearPendingSync() {
				SpinLockGuard guard(m_spinLock);
				m_hasPendingSync = false;

				if (m_dirty)
					m_dirty = hasPendingOperation();
			}

		private:
			bool hasPendingOperation() const {
				return 0 != m_numBytes || m_hasPendingSync;
			}

		private:
			using SpinLockGuard = std::lock_guard<utils::SpinLock>;
			utils::SpinLock m_spinLock;
			CompletionAwareBlockRangeConsumerFunc m_blockRangeConsumer;
			std::queue<ElementInfo> m_elements;
			size_t m_maxSize;
			size_t m_numBytes;
			bool m_hasPendingSync;
			bool m_dirty;
		};

		NodeInteractionResult ToNodeInteractionResult(ChainComparisonCode code) {
			switch (code) {
			case ChainComparisonCode::Remote_Reported_Equal_Chain_Score:
			case ChainComparisonCode::Remote_Reported_Lower_Chain_Score:
				return NodeInteractionResult::Neutral;
			default:
				return NodeInteractionResult::Failure;
			}
		}

		class RangeAggregator {
		public:
			void add(model::BlockRange&& range) {
				m_numBlocks += range.size();
				m_ranges.push_back(std::move(range));
			}

			auto merge() {
				return model::BlockRange::MergeRanges(std::move(m_ranges));
			}

			auto empty() {
				return 0 == m_numBlocks;
			}

			auto numBlocks() const {
				return m_numBlocks;
			}

		private:
			size_t m_numBlocks;
			std::vector<model::BlockRange> m_ranges;
		};

		auto CreateFutureSupplier(const chain::RemoteApi& remoteApi, const api::BlocksFromOptions& options) {
			return [&remoteApi, options](auto height) {
				return remoteApi.pChainApi->blocksFrom(height, options);
			};
		}

		NodeInteractionFuture CompleteChainBlocksFrom(RangeAggregator& rangeAggregator, UnprocessedElements& unprocessedElements) {
			if (rangeAggregator.empty())
				return thread::make_ready_future(NodeInteractionResult::Neutral);

			auto mergedRange = rangeAggregator.merge();
			auto addResult = unprocessedElements.add(std::move(mergedRange))
					? NodeInteractionResult::Success
					: NodeInteractionResult::Neutral;
			return thread::make_ready_future(std::move(addResult));
		}

		NodeInteractionFuture ChainBlocksFrom(
				const std::function<thread::future<model::BlockRange>(Height)>& futureSupplier,
				Height height,
				uint64_t forkDepth,
				const std::shared_ptr<RangeAggregator>& pRangeAggregator,
				UnprocessedElements& unprocessedElements) {
			return thread::compose(
					futureSupplier(height),
					[futureSupplier, forkDepth, pRangeAggregator, &unprocessedElements](auto&& blocksFuture) {
						try {
							auto range = blocksFuture.get();

							// if the range is empty, stop processing
							if (range.empty()) {
								CATAPULT_LOG(debug) << "peer returned 0 blocks";
								return CompleteChainBlocksFrom(*pRangeAggregator, unprocessedElements);
							}

							// if the range is not empty, continue processing
							auto endHeight = (--range.cend())->Height;
							CATAPULT_LOG(debug)
									<< "peer returned " << range.size()
									<< " blocks (heights " << range.cbegin()->Height << " - " << endHeight << ")";

							pRangeAggregator->add(std::move(range));
							if (forkDepth <= pRangeAggregator->numBlocks())
								return CompleteChainBlocksFrom(*pRangeAggregator, unprocessedElements);

							auto nextHeight = endHeight + Height(1);
							return ChainBlocksFrom(futureSupplier, nextHeight, forkDepth, pRangeAggregator, unprocessedElements);
						} catch (const catapult_runtime_error& e) {
							CATAPULT_LOG(debug) << "exception thrown while requesting blocks: " << e.what();
							return thread::make_ready_future(NodeInteractionResult::Failure);
						}
			});
		}

		class DefaultChainSynchronizer {
		public:
			// note: the synchronizer will only request config.MaxRollbackBlocks blocks so that even if the peer returns
			//       a chain part that is a fork of the real chain, that fork is still resolvable because it can be rolled back
			explicit DefaultChainSynchronizer(
					const std::shared_ptr<const api::ChainApi>& pLocalChainApi,
					const ChainSynchronizerConfiguration& config,
					const ShortHashesSupplier& shortHashesSupplier,
					const CompletionAwareBlockRangeConsumerFunc& blockRangeConsumer,
					const TransactionRangeConsumerFunc& transactionRangeConsumer)
					: m_pLocalChainApi(pLocalChainApi)
					, m_compareChainOptions(config.MaxBlocksPerSyncAttempt, config.MaxRollbackBlocks)
					, m_blocksFromOptions(config.MaxRollbackBlocks, config.MaxChainBytesPerSyncAttempt)
					, m_shortHashesSupplier(shortHashesSupplier)
					, m_transactionRangeConsumer(transactionRangeConsumer)
					, m_pUnprocessedElements(std::make_shared<UnprocessedElements>(
							blockRangeConsumer,
							3 * config.MaxChainBytesPerSyncAttempt))
			{}

		public:
			NodeInteractionFuture operator()(const RemoteApi& remoteApi) {
				if (!m_pUnprocessedElements->shouldStartSync())
					return thread::make_ready_future(NodeInteractionResult::Neutral);

				return thread::compose(
						thread::compose(
								compareChains(remoteApi),
								[this, &remoteApi](auto&& compareChainsFuture) -> NodeInteractionFuture {
									try {
										return this->syncWithPeer(remoteApi, compareChainsFuture.get());
									} catch (const catapult_runtime_error& e) {
										CATAPULT_LOG(debug) << "exception thrown while comparing chains: " << e.what();
										return thread::make_ready_future(NodeInteractionResult::Failure);
									}
								}),
						[&unprocessedElements = *m_pUnprocessedElements](auto&& nodeInteractionFuture) {
							// mark the current sync as completed
							unprocessedElements.clearPendingSync();
							return std::move(nodeInteractionFuture);
						});
			}

		private:
			// in case that there are no unprocessed elements in the disruptor, we do a normal synchronization round
			// else we bypass chain comparison and expand the existing chain part by pulling more blocks
			thread::future<CompareChainsResult> compareChains(const RemoteApi& remoteApi) {
				if (m_pUnprocessedElements->empty())
					return CompareChains(*m_pLocalChainApi, *remoteApi.pChainApi, m_compareChainOptions);

				CompareChainsResult result;
				result.Code = ChainComparisonCode::Remote_Is_Not_Synced;
				result.CommonBlockHeight = m_pUnprocessedElements->maxHeight();
				result.ForkDepth = 0;
				return thread::make_ready_future(std::move(result));
			}

			NodeInteractionFuture syncWithPeer(const RemoteApi& remoteApi, const CompareChainsResult& compareResult) const {
				switch (compareResult.Code) {
				case ChainComparisonCode::Remote_Reported_Equal_Chain_Score: {
					const auto& transactionRangeConsumer = m_transactionRangeConsumer;
					return remoteApi.pTransactionApi->unconfirmedTransactions(m_shortHashesSupplier())
						.then([&transactionRangeConsumer](auto&& transactionsFuture) -> NodeInteractionResult {
							try {
								auto range = transactionsFuture.get();
								CATAPULT_LOG(debug) << "peer returned " << range.size() << " unconfirmed transactions";
								transactionRangeConsumer(std::move(range));
								return NodeInteractionResult::Neutral;
							} catch (const catapult_runtime_error& e) {
								CATAPULT_LOG(debug) << "exception thrown while requesting unconfirmed transactions: " << e.what();
								return NodeInteractionResult::Failure;
							}
						});
				}

				case ChainComparisonCode::Remote_Is_Not_Synced:
					break;

				default:
					auto result = ToNodeInteractionResult(compareResult.Code);
					if (NodeInteractionResult::Failure == result)
						CATAPULT_LOG(warning) << "node interaction failed: " << compareResult.Code;

					return thread::make_ready_future(std::move(result));
				}

				CATAPULT_LOG(debug) << "pulling blocks from remote with common height " << compareResult.CommonBlockHeight;
				auto futureSupplier = CreateFutureSupplier(remoteApi, m_blocksFromOptions);
				auto pRangeAggregator = std::make_shared<RangeAggregator>();
				return ChainBlocksFrom(
						futureSupplier,
						compareResult.CommonBlockHeight + Height(1),
						compareResult.ForkDepth,
						pRangeAggregator,
						*m_pUnprocessedElements);
			}

		private:
			std::shared_ptr<const api::ChainApi> m_pLocalChainApi;
			CompareChainsOptions m_compareChainOptions;
			api::BlocksFromOptions m_blocksFromOptions;
			ShortHashesSupplier m_shortHashesSupplier;
			TransactionRangeConsumerFunc m_transactionRangeConsumer;
			std::shared_ptr<UnprocessedElements> m_pUnprocessedElements;
		};
	}

	ChainSynchronizer CreateChainSynchronizer(
			const std::shared_ptr<const api::ChainApi>& pLocalChainApi,
			const ChainSynchronizerConfiguration& config,
			const ShortHashesSupplier& shortHashesSupplier,
			const CompletionAwareBlockRangeConsumerFunc& blockRangeConsumer,
			const TransactionRangeConsumerFunc& transactionRangeConsumer) {
		auto pSynchronizer = std::make_shared<DefaultChainSynchronizer>(
				pLocalChainApi,
				config,
				shortHashesSupplier,
				blockRangeConsumer,
				transactionRangeConsumer);

		return [pSynchronizer](const auto& remoteApi) {
			// pSynchronizer is captured in the second lambda to compose, which extends its lifetime until
			// the async operation is complete
			return thread::compose(pSynchronizer->operator()(remoteApi), [pSynchronizer](auto&& future) {
				return std::move(future);
			});
		};
	}
}}