/**
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mongo/pch.h"
#include "mongo/db/repl/rs_sync.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/client.h"

namespace mongo {
namespace replset {
    BackgroundSync* BackgroundSync::s_instance = 0;
    boost::mutex BackgroundSync::s_mutex;

    BackgroundSyncInterface::~BackgroundSyncInterface() {}

    BackgroundSync::BackgroundSync() : _maxSize(200*1024*1024),
                                       _bufSize(0),
                                       _lastOpTimeFetched(0, 0),
                                       _lastH(0),
                                       _pause(true),
                                       _currentSyncTarget(NULL),
                                       _oplogMarkerTarget(NULL),
                                       _oplogMarker(true /* doHandshake */),
                                       _consumedOpTime(0, 0) {
    }

    BackgroundSync* BackgroundSync::get() {
        boost::unique_lock<boost::mutex> lock(s_mutex);
        if (s_instance == NULL && !inShutdown()) {
            s_instance = new BackgroundSync();
        }
        return s_instance;
    }

    void BackgroundSync::shutdown() {
        boost::unique_lock<boost::mutex> lock(s_mutex);
        if (s_instance == NULL) {
            return;
        }

        s_instance->_bufCond.notify_all();
    }

    void BackgroundSync::notifierThread() {
        Client::initThread("rsSyncNotifier");
        replLocalAuth();

        while (!inShutdown()) {
            bool clearTarget = false;

            if (!theReplSet) {
                sleepsecs(5);
                continue;
            }

            MemberState state = theReplSet->state();
            if (state.primary() || state.fatal() || state.startup()) {
                sleepsecs(5);
                continue;
            }

            try {
                markOplog();
            }
            catch (DBException &e) {
                clearTarget = true;
                log() << "replset tracking exception: " << e.getInfo() << rsLog;
                sleepsecs(1);
            }
            catch (std::exception &e2) {
                clearTarget = true;
                log() << "replset tracking error" << e2.what() << rsLog;
                sleepsecs(1);
            }

            if (clearTarget) {
                boost::unique_lock<boost::mutex> lock(_mutex);
                _oplogMarkerTarget = NULL;
            }
        }

        cc().shutdown();
    }

    void BackgroundSync::markOplog() {
        LOG(3) << "replset markOplog: " << _consumedOpTime << " " << theReplSet->lastOpTimeWritten << rsLog;

        if (!hasCursor()) {
            sleepsecs(1);
            return;
        }

        if (!_oplogMarker.moreInCurrentBatch()) {
            _oplogMarker.more();
        }

        if (!_oplogMarker.more()) {
            _oplogMarker.tailCheck();
            sleepsecs(1);
            return;
        }

        // if this member has written the op at optime T, we want to nextSafe up to and including T
        while (_consumedOpTime < theReplSet->lastOpTimeWritten && _oplogMarker.more()) {
            BSONObj temp = _oplogMarker.nextSafe();
            _consumedOpTime = temp["ts"]._opTime();
        }

        // next time through markOplog(), call more() to signal the sync target that we've synced T
    }

    bool BackgroundSync::hasCursor() {
        {
            // we don't need the global write lock yet, but it's needed by OplogReader::connect
            // so we take it preemptively to avoid deadlocking.
            Lock::GlobalWrite gwl;
            boost::unique_lock<boost::mutex> lock(_mutex);

            if (!_oplogMarkerTarget || _currentSyncTarget != _oplogMarkerTarget) {
                if (!_currentSyncTarget) {
                    return false;
                }

                log() << "replset setting oplog notifier to " << _currentSyncTarget->fullName() << rsLog;
                _oplogMarkerTarget = _currentSyncTarget;

                _oplogMarker.resetConnection();

                if (!_oplogMarker.connect(_oplogMarkerTarget->fullName())) {
                    LOG(1) << "replset could not connect to " << _oplogMarkerTarget->fullName() << rsLog;
                    _oplogMarkerTarget = NULL;
                    return false;
                }
            }
        }

        if (!_oplogMarker.haveCursor()) {
            BSONObj fields = BSON("ts" << 1);
            _oplogMarker.tailingQueryGTE(rsoplog, theReplSet->lastOpTimeWritten, &fields);
        }

        return _oplogMarker.haveCursor();
    }

    void BackgroundSync::producerThread() {
        Client::initThread("rsBackgroundSync");
        replLocalAuth();

        while (!inShutdown()) {
            if (!theReplSet) {
                log() << "replSet warning did not receive a valid config yet, sleeping 20 seconds " << rsLog;
                sleepsecs(20);
                continue;
            }

            try {
                _producerThread();
            }
            catch (DBException& e) {
                sethbmsg(str::stream() << "syncThread: " << e.toString());
                sleepsecs(10);
            }
            catch (...) {
                sethbmsg("unexpected exception in syncThread()");
                sleepsecs(60);
            }

            sleepsecs(1);
        }

        cc().shutdown();
    }

    void BackgroundSync::_producerThread() {
        MemberState state = theReplSet->state();

        // we want to pause when the state changes to primary
        if (state.primary()) {
            if (!_pause) {
                stop();
            }
            sleepsecs(1);
            return;
        }

        if (state.fatal() || state.startup()) {
            sleepsecs(5);
            return;
        }

        // if this member has an empty oplog, we cannot start syncing
        if (theReplSet->lastOpTimeWritten.isNull()) {
            sleepsecs(1);
            return;
        }
        // we want to unpause when we're no longer primary
        // start() also loads _lastOpTimeFetched, which we know is set from the "if"
        else if (_pause) {
            start();
        }

        produce();
    }

    void BackgroundSync::produce() {
        bool doHandshake = false;
        OplogReader r(doHandshake);

        // find a target to sync from the last op time written
        getOplogReader(r);

        // no server found
        {
            boost::unique_lock<boost::mutex> lock(_mutex);

            if (_currentSyncTarget == NULL) {
                sleepsecs(1);
                // if there is no one to sync from
                return;
            }

            r.tailingQueryGTE(rsoplog, _lastOpTimeFetched);
        }

        // if target cut connections between connecting and querying (for
        // example, because it stepped down) we might not have a cursor
        if (!r.haveCursor()) {
            return;
        }

        uassert(1000, "replSet source for syncing doesn't seem to be await capable -- is it an older version of mongodb?", r.awaitCapable() );

        if (isRollbackRequired(r)) {
            stop();
            return;
        }

        while (!inShutdown()) {
            while (!inShutdown()) {
                // if the buffer is full, wait for items to be removed
                if (_bufSize >= _maxSize) {
                    boost::unique_lock<boost::mutex> lock(_mutex);
                    _bufCond.wait(lock);

                    // if we produce [peanut, ..., peanut, whale] we'll have to consume more than
                    // one peanut before we're below _maxSize, so keep waiting until we are
                    break;
                }

                if (!r.moreInCurrentBatch()) {
                    if (theReplSet->gotForceSync()) {
                        return;
                    }

                    if (theReplSet->isPrimary()) {
                        return;
                    }

                    {
                        boost::unique_lock<boost::mutex> lock(_mutex);
                        if (!_currentSyncTarget || !_currentSyncTarget->hbinfo().hbstate.readable()) {
                            return;
                        }
                    }

                    r.more();
                }

                if (!r.more())
                    break;

                BSONObj o = r.nextSafe();

                {
                    boost::unique_lock<boost::mutex> lock(_mutex);
                    _buffer.push(o.getOwned());
                    _bufSize += o.objsize();
                    _lastH = o["h"].numberLong();
                    _lastOpTimeFetched = o["ts"]._opTime();
                }
            } // end while

            {
                boost::unique_lock<boost::mutex> lock(_mutex);
                if (_pause || !_currentSyncTarget || !_currentSyncTarget->hbinfo().hbstate.readable()) {
                    return;
                }
            }

            r.tailCheck();
            if( !r.haveCursor() ) {
                LOG(1) << "replSet end syncTail pass" << rsLog;
                return;
            }

            // looping back is ok because this is a tailable cursor
        }
    }

    BSONObj* BackgroundSync::peek() {
        boost::unique_lock<boost::mutex> lock(_mutex);

        if (_currentSyncTarget != _oplogMarkerTarget &&
            _currentSyncTarget != NULL) {
            _oplogMarkerTarget = NULL;
        }

        if (_buffer.empty()) {
            // it should already be 0, but just in case
            _bufSize = 0;
            return NULL;
        }

        return &_buffer.front();
    }

    void BackgroundSync::consume() {
        boost::unique_lock<boost::mutex> lock(_mutex);
        BSONObj& front = _buffer.front();

        // remove from the queue first, in case catchup goes wrong
        _bufSize -= front.objsize();
        _buffer.pop();

        // wake up producer, if it's waiting for docs to be consumed
        _bufCond.notify_one();
    }

    bool BackgroundSync::isStale(OplogReader& r, BSONObj& remoteOldestOp) {
        remoteOldestOp = r.findOne(rsoplog, Query());
        OpTime remoteTs = remoteOldestOp["ts"]._opTime();
        DEV {
            log() << "replSet remoteOldestOp:    " << remoteTs.toStringLong() << rsLog;
            log() << "replSet lastOpTimeFetched: " << _lastOpTimeFetched.toStringLong() << rsLog;
        }
        LOG(3) << "replSet remoteOldestOp: " << remoteTs.toStringLong() << rsLog;

        {
            boost::unique_lock<boost::mutex> lock(_mutex);

            if (_lastOpTimeFetched >= remoteTs) {
                return false;
            }
        }

        return true;
    }

    void BackgroundSync::getOplogReader(OplogReader& r) {
        Member *target = NULL, *stale = NULL;
        BSONObj oldest;

        // then we're initial syncing and we're still waiting for this to be set
        {
            boost::unique_lock<boost::mutex> lock(_mutex);
            if (_lastOpTimeFetched.isNull()) {
                _currentSyncTarget = NULL;
                return;
            }
        }

        verify(r.conn() == NULL);

        while ((target = theReplSet->getMemberToSyncTo()) != NULL) {
            string current = target->fullName();

            if (!r.connect(current)) {
                log(2) << "replSet can't connect to " << current << " to read operations" << rsLog;
                r.resetConnection();
                theReplSet->veto(current);
                continue;
            }

            if (isStale(r, oldest)) {
                r.resetConnection();
                theReplSet->veto(current, 600);
                stale = target;
                continue;
            }

            // if we made it here, the target is up and not stale
            {
                boost::unique_lock<boost::mutex> lock(_mutex);
                _currentSyncTarget = target;
            }

            return;
        }

        // the only viable sync target was stale
        if (stale) {
            theReplSet->goStale(stale, oldest);
            sleepsecs(120);
        }

        {
            boost::unique_lock<boost::mutex> lock(_mutex);
            _currentSyncTarget = NULL;
        }
    }

    bool BackgroundSync::isRollbackRequired(OplogReader& r) {
        string hn = r.conn()->getServerAddress();

        if (!r.more()) {
            try {
                BSONObj theirLastOp = r.getLastOp(rsoplog);
                if (theirLastOp.isEmpty()) {
                    log() << "replSet error empty query result from " << hn << " oplog" << rsLog;
                    sleepsecs(2);
                    return true;
                }
                OpTime theirTS = theirLastOp["ts"]._opTime();
                if (theirTS < theReplSet->lastOpTimeWritten) {
                    log() << "replSet we are ahead of the primary, will try to roll back" << rsLog;
                    theReplSet->syncRollback(r);
                    return true;
                }
                /* we're not ahead?  maybe our new query got fresher data.  best to come back and try again */
                log() << "replSet syncTail condition 1" << rsLog;
                sleepsecs(1);
            }
            catch(DBException& e) {
                log() << "replSet error querying " << hn << ' ' << e.toString() << rsLog;
                sleepsecs(2);
            }
            return true;
        }

        BSONObj o = r.nextSafe();
        OpTime ts = o["ts"]._opTime();
        long long h = o["h"].numberLong();
        if( ts != theReplSet->lastOpTimeWritten || h != theReplSet->lastH ) {
            log() << "replSet our last op time written: " << theReplSet->lastOpTimeWritten.toStringPretty() << rsLog;
            log() << "replset source's GTE: " << ts.toStringPretty() << rsLog;
            theReplSet->syncRollback(r);
            return true;
        }

        return false;
    }

    Member* BackgroundSync::getSyncTarget() {
        boost::unique_lock<boost::mutex> lock(_mutex);
        return _currentSyncTarget;
    }

    void BackgroundSync::stop() {
        boost::unique_lock<boost::mutex> lock(_mutex);

        int popped = 0;

        _pause = true;
        _currentSyncTarget = NULL;
        _lastOpTimeFetched = OpTime(0,0);
        _lastH = 0;

        // get rid of pending ops
        while (!_buffer.empty()) {
            _buffer.pop();
            popped++;
        }
        _bufSize = 0;

        if (popped > 0) {
            log() << "replset " << popped << " ops were not applied from buffer, this should "
                  << "cause a rollback on the former primary" << rsLog;
        }
    }

    void BackgroundSync::start() {
        boost::unique_lock<boost::mutex> lock(_mutex);
        _pause = false;
        massert(16160, "going to start syncing, but buffer is not empty", _buffer.empty() && _bufSize == 0);

        // reset _last fields with current data
        _lastOpTimeFetched = theReplSet->lastOpTimeWritten;
        _lastH = theReplSet->lastH;

        LOG(1) << "replset bgsync fetch queue set to: " << _lastOpTimeFetched << " " << _lastH << rsLog;
   }

} // namespace replset
} // namespace mongo
