#pragma once

#include "common/options.h"
#include "data/batch_stats.h"
#include "data/rng_engine.h"
#include "training/training_state.h"
#include "data/iterator_facade.h"
#include "3rd_party/threadpool.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <queue>

namespace marian {
namespace data {

// Iterator over batches generated by a BatchGenerator. Mean to be the only
// interface to create batches.
template <class BatchGenerator>
class BatchIterator : public IteratorFacade<BatchIterator<BatchGenerator>,
                                            typename BatchGenerator::BatchPtr> {
private:
  BatchGenerator* bg_;
  typename BatchGenerator::BatchPtr current_;

  friend BatchGenerator;

  // private, only BatchGenerator is allowed to create pointers
  // hence friend class above.
  BatchIterator(BatchGenerator* bg, typename BatchGenerator::BatchPtr current)
  : bg_(bg), current_(current) {}

public:
  virtual bool equal(const BatchIterator& other) const override {
    // iterators are only equal if they point at the same batch or both have nullptr
    return current_ == other.current_;
  }

  // Just returns the batch pointer
  virtual const typename BatchGenerator::BatchPtr& dereference() const override {
    return current_;
  }

  // sets current pointer to the next batch pointer, will be nullptr when no
  // batches are available. This will evaluate to false in a for loop.
  virtual void increment() override {
    current_ = bg_->next();
  };
};

template <class DataSet>
class BatchGenerator : public RNGEngine {
public:
  typedef typename DataSet::batch_ptr BatchPtr;

  typedef typename DataSet::Sample Sample;
  typedef std::vector<Sample> Samples;

  typedef BatchIterator<BatchGenerator> iterator;
  friend iterator;

protected:
  Ptr<DataSet> data_;
  Ptr<Options> options_;
  bool restored_{false};
  bool shuffle_;

private:
  Ptr<BatchStats> stats_;

  // state of fetching
  std::deque<BatchPtr> bufferedBatches_; // current swath of batches that next() reads from

  // state of reading
  typename DataSet::iterator current_;
  bool newlyPrepared_{ true }; // prepare() was just called: we need to reset current_  --@TODO: can we just reset it directly?

  // variables for multi-threaded pre-fetching
  mutable ThreadPool threadPool_; // (we only use one thread, but keep it around)
  std::future<std::deque<BatchPtr>> futureBufferedBatches_; // next swath of batches is returned via this

  // this runs on a bg thread; sequencing is handled by caller, but locking is done in here
  std::deque<BatchPtr> fetchBatches() {
    typedef typename Sample::value_type Item;
    auto itemCmp = [](const Item& sa, const Item& sb) {
      // sort by element length, not content
      return sa.size() < sb.size();
    };

    auto cmpSrc = [itemCmp](const Sample& a, const Sample& b) {
      return std::lexicographical_compare(
          a.begin(), a.end(), b.begin(), b.end(), itemCmp);
    };

    auto cmpTrg = [itemCmp](const Sample& a, const Sample& b) {
      return std::lexicographical_compare(
          a.rbegin(), a.rend(), b.rbegin(), b.rend(), itemCmp);
    };

    auto cmpNone = [](const Sample& a, const Sample& b) {
      // sort by address, so we have something to work with
      return &a < &b;
    };

    typedef std::function<bool(const Sample&, const Sample&)> cmp_type;
    typedef std::priority_queue<Sample, Samples, cmp_type> sample_queue;

    std::unique_ptr<sample_queue> maxiBatch; // priority queue, shortest first

    if(options_->has("maxi-batch-sort")) {
      if(options_->get<std::string>("maxi-batch-sort") == "src")
        maxiBatch.reset(new sample_queue(cmpSrc));
      else if(options_->get<std::string>("maxi-batch-sort") == "none")
        maxiBatch.reset(new sample_queue(cmpNone));
      else
        maxiBatch.reset(new sample_queue(cmpTrg));
    } else {
      maxiBatch.reset(new sample_queue(cmpNone));
    }

    size_t maxBatchSize = options_->get<int>("mini-batch");
    size_t maxSize = maxBatchSize * options_->get<int>("maxi-batch");

    // consume data from corpus into maxi-batch (single sentences)
    // sorted into specified order (due to queue)
    if(newlyPrepared_) {
      current_ = data_->begin();
      newlyPrepared_ = false;
    } else {
      if(current_ != data_->end())
        ++current_;
    }
    size_t sets = 0;
    while(current_ != data_->end() && maxiBatch->size() < maxSize) { // loop over data
      maxiBatch->push(*current_);
      sets = current_->size();
        // do not consume more than required for the maxi batch as this causes
        // that line-by-line translation is delayed by one sentence
        bool last = maxiBatch->size() == maxSize;
      if(!last)
        ++current_; // this actually reads the next line and pre-processes it
    }
    size_t numSentencesRead = maxiBatch->size();

    // construct the actual batches and place them in the queue
    Samples batchVector;
    size_t currentWords = 0;
    std::vector<size_t> lengths(sets, 0); // records maximum length observed within current batch

    std::deque<BatchPtr> tempBatches;

    // process all loaded sentences in order of increasing length
    // @TODO: we could just use a vector and do a sort() here; would make the cost more explicit
    const size_t mbWords = options_->get<size_t>("mini-batch-words", 0);
    const bool useDynamicBatching = options_->has("mini-batch-fit");
    BatchStats::const_iterator cachedStatsIter;
    if (stats_)
      cachedStatsIter = stats_->begin();
    while(!maxiBatch->empty()) { // while there are sentences in the queue
      // push item onto batch
      batchVector.push_back(maxiBatch->top());
      maxiBatch->pop(); // fetch next-shortest

      // have we reached sufficient amount of data to form a batch?
      bool makeBatch;
      if(useDynamicBatching && stats_) { // batch size based on dynamic batching
        for(size_t i = 0; i < sets; ++i)
          if(batchVector.back()[i].size() > lengths[i])
            lengths[i] = batchVector.back()[i].size(); // record max lengths so far

        maxBatchSize = stats_->findBatchSize(lengths, cachedStatsIter);
        // this optimization makes no difference indeed
#if 1     // sanity check: would we find the same entry if searching from the start?
        auto it = stats_->lower_bound(lengths);
        auto maxBatchSize1 = stats_->findBatchSize(lengths, it);
        ABORT_IF(maxBatchSize != maxBatchSize1, "findBatchSize iter caching logic is borked");
#endif
        makeBatch = batchVector.size() >= maxBatchSize;
        // if last added sentence caused a bump then we likely have bad padding, so rather move it into the next batch
        if(batchVector.size() > maxBatchSize) {
          maxiBatch->push(batchVector.back());
          batchVector.pop_back();
        }
      }
      else if(mbWords > 0) {
        currentWords += batchVector.back()[0].size(); // count words based on first stream =source  --@TODO: shouldn't we count based on labels?
        makeBatch = currentWords > mbWords; // Batch size based on sentences
      }
      else
        makeBatch = batchVector.size() == maxBatchSize; // Batch size based on words

      // if we reached the desired batch size then create a real batch
      if(makeBatch) {
        tempBatches.push_back(data_->toBatch(batchVector));

        // prepare for next batch
        batchVector.clear();
        currentWords = 0;
        lengths.assign(sets, 0);
        if (stats_)
          cachedStatsIter = stats_->begin();
      }
    }

    // turn rest into batch
    // @BUGBUG: This can create a very small batch, which with ce-mean-words can artificially
    // inflate the contribution of the sames in the batch, causing instability.
    // I think a good alternative would be to carry over the left-over sentences into the next round.
    if(!batchVector.empty())
      tempBatches.push_back(data_->toBatch(batchVector));

    // Shuffle the batches
    if(shuffle_) {
      std::shuffle(tempBatches.begin(), tempBatches.end(), eng_);
    }
    double totalSent{}, totalLabels{};
    for (auto& b : tempBatches) {
      totalSent += (double)b->size();
      totalLabels += (double)b->words(-1);
    }
    auto totalDenom = tempBatches.empty() ? 1 : tempBatches.size(); // (make 0/0 = 0)
    LOG(debug, "[data] fetched {} batches with {} sentences. Per batch: {} sentences, {} labels.",
        tempBatches.size(), numSentencesRead,
        (double)totalSent / (double)totalDenom, (double)totalLabels / (double)totalDenom);
    return tempBatches;
  }

  // this starts fillBatches() as a background operation
  void fetchBatchesAsync() {
    ABORT_IF(futureBufferedBatches_.valid(), "attempted to restart futureBufferedBatches_ while still running");
    futureBufferedBatches_ = threadPool_.enqueue([this]() {
      return fetchBatches();
    });
  }

  BatchPtr next() {
    if(bufferedBatches_.empty()) {
      // out of data: need to get next batch from background thread
      // We only get here if the future has been scheduled to run; it must be valid.
      ABORT_IF(!futureBufferedBatches_.valid(), "attempted to wait for futureBufferedBatches_ when none pending");
      bufferedBatches_ = std::move(futureBufferedBatches_.get());
      // if bg thread returns an empty swath, we hit the end of the epoch
      if (bufferedBatches_.empty()) {
        return nullptr;
      }
      // and kick off the next bg operation
      fetchBatchesAsync();
    }
    auto batch = bufferedBatches_.front();
    bufferedBatches_.pop_front();
    return batch;
  }

public:

  BatchGenerator(Ptr<DataSet> data,
                 Ptr<Options> options,
                 Ptr<BatchStats> stats = nullptr)
    : data_(data), options_(options), stats_(stats), threadPool_(1) { }

  ~BatchGenerator() {
    if (futureBufferedBatches_.valid()) // bg thread holds a reference to 'this',
      futureBufferedBatches_.get();     // so must wait for it to complete
  }

  iterator begin() {
    return iterator(this, next());
  }

  iterator end() {
    return iterator(this, nullptr);
  }

  // @TODO: get rid of this function, begin() or constructor should figure this out
  void prepare(bool shuffle) {
    if(restored_) { // state was just restored, restore() calls prepare()
      restored_ = false;
      return;
    }
    if(shuffle)
      data_->shuffle();
    else
      data_->reset();
    newlyPrepared_ = true;

    // @TODO: solve this better, maybe use options
    // => for this to work best, we need to replace --no-shuffle by --shuffle
    // which is true by default for train, false otherwise, or explicitly set
    // --no-shuffle=true by default for translate, validate, score etc. [UG]
    // for the time begin, let's stick with the explicit function parameter
    // (I've disabled the default value, because it's prone to cause problems
    // sooner or later; callers should know if they want to shuffle or not).
    shuffle_ = shuffle;

    // start the background pre-fetch operation
    fetchBatchesAsync();
  }

  // Used to restore the state of a BatchGenerator after
  // an interrupted and resumed training.
  bool restore(Ptr<TrainingState> state, bool shuffle) {
    if(state->epochs == 1 && state->batchesEpoch == 0)
      return false;
    if (options_->get<bool>("no-restore-corpus"))
      return false;

    LOG(info,
        "[data] Restoring the corpus state to epoch {}, batch {}",
        state->epochs,
        state->batches);

    if(state->epochs > 1) {
      data_->restore(state);
      setRNGState(state->seedBatch);
    }

    prepare(shuffle);
    for(size_t i = 0; i < state->batchesEpoch; ++i)
      next();

    restored_ = true;
    return true;
  }

  // this is needed for dynamic MB scaling. Returns 0 if size is not known in words.
  size_t estimateTypicalTrgBatchWords() const {
    const size_t mbWords = options_->get<size_t>("mini-batch-words", 0);
    const bool useDynamicBatching = options_->has("mini-batch-fit");
    if (useDynamicBatching && stats_)
      return stats_->estimateTypicalTrgWords();
    else if (mbWords)
      return mbWords;
    else
      return 0;
  }
};

class CorpusBatchGenerator : public BatchGenerator<CorpusBase>,
                             public TrainingObserver {
public:
  CorpusBatchGenerator(Ptr<CorpusBase> data,
                       Ptr<Options> options,
                       Ptr<BatchStats> stats = nullptr)
      : BatchGenerator(data, options, stats) {}

  void actAfterEpoch(TrainingState& state) override {
    state.seedBatch = getRNGState();
    state.seedCorpus = data_->getRNGState();
  }
};
}  // namespace data
}  // namespace marian
