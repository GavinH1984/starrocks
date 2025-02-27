// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/fe/fe-core/src/main/java/org/apache/doris/catalog/MetaObject.java

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package com.starrocks.catalog;

import com.starrocks.common.FeMetaVersion;
import com.starrocks.common.io.Writable;
import com.starrocks.server.GlobalStateMgr;

import java.io.DataInput;
import java.io.DataOutput;
import java.io.IOException;
import java.util.zip.Adler32;

public class MetaObject implements Writable {

    protected long signature;
    protected long lastCheckTime; // last check consistency time

    public MetaObject() {
        signature = -1L;
        lastCheckTime = -1L;
    }

    // implement this in derived class
    public int getSignature(int signatureVersion) {
        Adler32 adler32 = new Adler32();
        adler32.update(signatureVersion);
        return Math.abs((int) adler32.getValue());
    }

    public long getLastCheckTime() {
        return lastCheckTime;
    }

    public void setLastCheckTime(long lastCheckTime) {
        this.lastCheckTime = lastCheckTime;
    }

    @Override
    public void write(DataOutput out) throws IOException {
        out.writeLong(signature);
        out.writeLong(lastCheckTime);
    }

    public void readFields(DataInput in) throws IOException {
        if (GlobalStateMgr.getCurrentStateJournalVersion() >= FeMetaVersion.VERSION_22) {
            this.signature = in.readLong();
        }

        if (GlobalStateMgr.getCurrentStateJournalVersion() >= 6) {
            this.lastCheckTime = in.readLong();
        }
    }

}
