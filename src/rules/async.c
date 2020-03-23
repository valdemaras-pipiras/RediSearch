#include "util/arr.h"
#include "rules.h"
#include "module.h"
#include "indexer.h"
#include <sys/time.h>

static void *aiThreadInit(void *privdata);

AsyncIndexQueue *AIQ_Create(size_t interval, size_t batchSize) {
  AsyncIndexQueue *q = rm_calloc(1, sizeof(*q));

  q->interval = interval;
  q->indexBatchSize = batchSize;
  q->pending = array_new(SpecDocQueue *, 8);
  pthread_mutex_init(&q->lock, NULL);
  pthread_cond_init(&q->cond, NULL);
  // didn't find pthread.c when in function AsyncIndexCtx_Create
  pthread_create(&q->aiThread, NULL, aiThreadInit, q);
  pthread_detach(q->aiThread);
  return q;
}

void AIQ_Destroy(AsyncIndexQueue *aq) {
  aq->state = AIQ_S_CANCELLED;
  pthread_cond_signal(&aq->cond);
  pthread_join(aq->aiThread, NULL);
  pthread_mutex_destroy(&aq->lock);
  pthread_cond_destroy(&aq->cond);
  array_free(aq->pending);
  rm_free(aq);
}

void AIQ_Submit(AsyncIndexQueue *aq, IndexSpec *spec, MatchAction *result, RuleKeyItem *item) {
  // submit to queue
  // 1. Create a queue per index
  // 2. Add `item` to queue
  // Somewhere else
  // 3. As a callback, index items into the index
  // 4. Remove items from queue

  // Queue indexes with documents to be indexed
  // Queue documents to be indexed for each index
  // Schedule

  // index points to a retained ptr at SchemaRules_g
  RuleIndexableDocument *rid = rm_calloc(1, sizeof(*rid));
  rid->kstr = item->kstr;
  rid->iia = result->attrs;
  RedisModule_RetainString(NULL, rid->kstr);
  SpecDocQueue *dq = spec->queue;
  if (!dq) {
    dq = SpecDocQueue_Create(spec);
  }

  pthread_mutex_lock(&aq->lock);
  int rv = dictAdd(dq->entries, rid->kstr, rid);
  if (rv != DICT_OK) {
    // item already exists?
    pthread_mutex_unlock(&aq->lock);
    rm_free(rid);
    RedisModule_FreeString(NULL, item->kstr);
    return;
  }
  int flags = dq->state;
  size_t nqueued = dictSize(dq->entries);

  if ((flags & (SDQ_S_PENDING | SDQ_S_PROCESSING)) == 0) {
    printf("adding to list of pending indexes...\n");
    // the pending flag isn't set yet, and we aren't processing either,
    // go ahead and add this to the pending array
    aq->pending = array_append(aq->pending, dq);
    dq->state |= SDQ_S_PENDING;
    IndexSpec_Incref(spec);
  }

  pthread_mutex_unlock(&aq->lock);
  if ((flags & SDQ_S_PROCESSING) == 0 && nqueued >= aq->indexBatchSize) {
    printf("signalling thread..\n");
    pthread_cond_signal(&aq->cond);
  }
}

static void ms2ts(struct timespec *ts, unsigned long ms) {
  ts->tv_sec = ms / 1000;
  ts->tv_nsec = (ms % 1000) * 1000000;
}

static void freeCallback(RSAddDocumentCtx *ctx, void *unused) {
  ACTX_Free(ctx);
}

static void indexBatch(AsyncIndexQueue *aiq, SpecDocQueue *dq, dict *entries) {
  printf("indexBatch!\n");
  Indexer idxr = {0};
  IndexSpec *sp = dq->spec;
  RedisSearchCtx sctx = SEARCH_CTX_STATIC(RSDummyContext, dq->spec);
  Indexer_Init(&idxr, &sctx);
  dictIterator *iter = dictGetIterator(entries);
  dictEntry *e = NULL;
  while ((e = dictNext(iter)) && !(sp->flags & Index_Deleted)) {
    RuleIndexableDocument *rid = e->v.val;
    QueryError err = {0};
    RuleKeyItem rki = {.kstr = rid->kstr};

    RedisModule_ThreadSafeContextLock(RSDummyContext);
    RSAddDocumentCtx *aCtx = SchemaRules_InitACTX(RSDummyContext, sp, &rki, &rid->iia, &err);
    if (!aCtx) {
      printf("Couldn't index (%s): %s\n", RedisModule_StringPtrLen(rid->kstr, NULL),
             QueryError_GetError(&err));
      continue;
    }
    RedisModule_ThreadSafeContextUnlock(RSDummyContext);

    if (Indexer_Add(&idxr, aCtx) != REDISMODULE_OK) {
      printf("Couldn't index (%s): %s\n", RedisModule_StringPtrLen(rid->kstr, NULL),
             QueryError_GetError(&aCtx->status));
      ACTX_Free(aCtx);
    }
    RedisModule_FreeString(NULL, rid->kstr);
    rm_free(rid);
    // deal with Key when applicable
  }
  dictReleaseIterator(iter);

  RedisModule_ThreadSafeContextLock(RSDummyContext);
  if (sp->flags & Index_Deleted) {
    Indexer_Iterate(&idxr, freeCallback, NULL);
  } else {
    Indexer_Index(&idxr, freeCallback, NULL);
  }
  Indexer_Destroy(&idxr);
  RedisModule_ThreadSafeContextUnlock(RSDummyContext);

  // now that we're done, lock the dq and see if we need to place it back into
  // pending again:
  pthread_mutex_lock(&aiq->lock);
  dq->state &= ~SDQ_S_PROCESSING;
  dq->nactive = 0;

  if (dictSize(dq->entries)) {
    dq->state = SDQ_S_PENDING;
    aiq->pending = array_append(aiq->pending, dq);
  } else {
    IndexSpec_Decref(dq->spec);
  }

  pthread_mutex_unlock(&aiq->lock);
}

static int sortPending(const void *a, const void *b) {
  const SpecDocQueue *qa = a, *qb = b;
  return dictSize(qa->entries) - dictSize(qb->entries);
}

static void *aiThreadInit(void *privdata) {
  AsyncIndexQueue *q = privdata;
  struct timespec base_ts = {0};
  ms2ts(&base_ts, q->interval);
  dict *newdict = dictCreate(&dictTypeHeapRedisStrings, NULL);

  while (1) {
    /**
     * Wait until the specified interval expires, OR when we are signalled with
     * a given amount of items.
     */
    if (q->state == AIQ_S_CANCELLED) {
      break;
    }
    pthread_mutex_lock(&q->lock);
    size_t nq;

    while ((nq = array_len(q->pending)) == 0) {
      struct timespec ts = {0};
      struct timeval tv;
      gettimeofday(&tv, NULL);
      ts.tv_sec = tv.tv_sec + base_ts.tv_sec;
      ts.tv_nsec = (tv.tv_usec * 1000) + base_ts.tv_nsec;
      int rv = pthread_cond_timedwait(&q->cond, &q->lock, &ts);
      assert(rv != EINVAL);
      // todo: cancel/exit scenarios?
    }

    // sort in ascending order. The queue with the fewest items comes first.
    // this makes it easier to delete items when done.
    qsort(q->pending, nq, sizeof(*q->pending), sortPending);
    SpecDocQueue *dq = q->pending[nq - 1];
    array_del_fast(q->pending, nq - 1);
    dict *oldEntries = dq->entries;
    dq->entries = newdict;
    dq->nactive = dictSize(oldEntries);
    dq->state = SDQ_S_PROCESSING;

    newdict = NULL;
    pthread_mutex_unlock(&q->lock);
    indexBatch(q, dq, oldEntries);

    if (!newdict) {
      newdict = dictCreate(&dictTypeHeapRedisStrings, NULL);
    }
  }
  if (newdict) {
    dictRelease(newdict);
  }
  return NULL;
}

ssize_t SchemaRules_GetPendingCount(const IndexSpec *spec) {
  if (!spec->queue) {
    printf("No queue. returning -1\n");
    return -1;
  }
  SpecDocQueue *dq = spec->queue;
  AsyncIndexQueue *aiq = asyncQueue_g;
  ssize_t ret;
  pthread_mutex_lock(&aiq->lock);
  pthread_mutex_lock(&dq->lock);

  ret = dq->nactive + dictSize(dq->entries);

  pthread_mutex_unlock(&dq->lock);
  pthread_mutex_unlock(&aiq->lock);
  return ret;
}

void SDQ_RemoveDoc(SpecDocQueue *sdq, AsyncIndexQueue *aiq, RedisModuleString *keyname) {
}