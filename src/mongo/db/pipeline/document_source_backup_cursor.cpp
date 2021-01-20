/*======
This file is part of Percona Server for MongoDB.

Copyright (C) 2021-present Percona and/or its affiliates. All rights reserved.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the Server Side Public License, version 1,
    as published by MongoDB, Inc.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    Server Side Public License for more details.

    You should have received a copy of the Server Side Public License
    along with this program. If not, see
    <http://www.mongodb.com/licensing/server-side-public-license>.

    As a special exception, the copyright holders give permission to link the
    code of portions of this program with the OpenSSL library under certain
    conditions as described in each individual source file and distribute
    linked combinations including the program with the OpenSSL library. You
    must comply with the Server Side Public License in all respects for
    all of the code used other than as permitted herein. If you modify file(s)
    with this exception, you may extend this exception to your version of the
    file(s), but you are not obligated to do so. If you do not wish to do so,
    delete this exception statement from your version. If you delete this
    exception statement from all source files in the program, then also delete
    it in the license file.
======= */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_backup_cursor.h"

namespace mongo {

using boost::intrusive_ptr;

REGISTER_DOCUMENT_SOURCE(backupCursor,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceBackupCursor::createFromBson);

const char* DocumentSourceBackupCursor::getSourceName() const {
    return kStageName.rawData();
}

Value DocumentSourceBackupCursor::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    if (explain) {
        BSONObjBuilder builder;
        // TODO: reimplement
        //_expression->serialize(&builder);
        return Value(DOC(getSourceName() << Document(builder.obj())));
    }
    return Value(DOC(getSourceName() << 0));
}
DocumentSource::GetNextResult DocumentSourceBackupCursor::doGetNext() {
    // TODO: implement
    return GetNextResult::makeEOF();
    auto nextInput = pSource->getNext();
    return nextInput;
}

intrusive_ptr<DocumentSourceBackupCursor> DocumentSourceBackupCursor::create(
    const BSONObj& filter, const intrusive_ptr<ExpressionContext>& expCtx) {
    intrusive_ptr<DocumentSourceBackupCursor> backupCursor(
        new DocumentSourceBackupCursor(filter, expCtx));
    return backupCursor;
}

intrusive_ptr<DocumentSource> DocumentSourceBackupCursor::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(15959, "the match filter must be an expression in an object", elem.type() == Object);

    return DocumentSourceBackupCursor::create(elem.Obj(), pExpCtx);
}

DocumentSourceBackupCursor::DocumentSourceBackupCursor(
    const BSONObj& query, const intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSource(kStageName, expCtx) {
    // TODO: reimplement
    // rebuild(query);
}
}  // namespace mongo
