/*jshint globalstrict:false, strict:false, maxlen: 500 */
/*global assertUndefined, assertEqual, assertTrue, assertFalse, assertNotNull, fail, db._query */

////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2017 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

var jsunity = require("jsunity");
var db = require("@arangodb").db;
var analyzers = require("@arangodb/analyzers");

////////////////////////////////////////////////////////////////////////////////
/// @brief test suite
////////////////////////////////////////////////////////////////////////////////

function iResearchFeatureAqlTestSuite () {
  return {
    setUpAll : function () {
    },

    tearDownAll : function () {
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief IResearchAnalyzerFeature tests
////////////////////////////////////////////////////////////////////////////////
    testAnalyzersCollectionPresent: function() {
      let dbName = "analyzersCollTestDb";
      try { db._dropDatabase(dbName); } catch (e) {}
      db._createDatabase(dbName);
      db._useDatabase(dbName);
      assertTrue(null !== db._collection("_analyzers"));
      db._useDatabase("_system");
      db._dropDatabase(dbName);
    },

    testAnalyzersInvalidPropertiesDiscarded : function() {
      {
        try {analyzers.remove("normPropAnalyzer"); } catch (e) {}
        assertEqual(0, db._analyzers.count());
        let analyzer = analyzers.save("normPropAnalyzer", "norm", { "locale":"en", "invalid_param":true});
        assertEqual(1, db._analyzers.count());
        assertTrue(null != analyzer);
        assertTrue(null == analyzer.properties.invalid_param);
        analyzers.remove("normPropAnalyzer", true);
        assertEqual(0, db._analyzers.count());
      }
      {
        try {analyzers.remove("textPropAnalyzer"); } catch (e) {}
        assertEqual(0, db._analyzers.count());
        let analyzer = analyzers.save("textPropAnalyzer", "text", {"stopwords" : [], "locale":"en", "invalid_param":true});
        assertEqual(1, db._analyzers.count());
        assertTrue(null != analyzer);
        assertTrue(null == analyzer.properties.invalid_param);
        analyzers.remove("textPropAnalyzer", true);
        assertEqual(0, db._analyzers.count());
      }
      {
        try {analyzers.remove("delimiterPropAnalyzer"); } catch (e) {}
        assertEqual(0, db._analyzers.count());
        let analyzer = analyzers.save("delimiterPropAnalyzer", "delimiter", { "delimiter":"|", "invalid_param":true});
        assertEqual(1, db._analyzers.count());
        assertTrue(null != analyzer);
        assertTrue(null == analyzer.properties.invalid_param);
        analyzers.remove("delimiterPropAnalyzer", true);
        assertEqual(0, db._analyzers.count());
      }
      {
        try {analyzers.remove("stemPropAnalyzer"); } catch (e) {}
        assertEqual(0, db._analyzers.count());
        let analyzer = analyzers.save("stemPropAnalyzer", "stem", { "locale":"en", "invalid_param":true});
        assertEqual(1, db._analyzers.count());
        assertTrue(null != analyzer);
        assertTrue(null == analyzer.properties.invalid_param);
        analyzers.remove("stemPropAnalyzer", true);
        assertEqual(0, db._analyzers.count());
      }
      {
        try {analyzers.remove("ngramPropAnalyzer"); } catch (e) {}
        assertEqual(0, db._analyzers.count());
        let analyzer = analyzers.save("ngramPropAnalyzer", "ngram", { "min":1, "max":5, "preserveOriginal":true, "invalid_param":true});
        assertEqual(1, db._analyzers.count());
        assertTrue(null != analyzer);
        assertTrue(null == analyzer.properties.invalid_param);
        analyzers.remove("ngramPropAnalyzer", true);
        assertEqual(0, db._analyzers.count());
      }
    },
    testAnalyzerRemovalWithDatabaseName_InSystem: function() {
      let dbName = "analyzerWrongDbName1";
      db._useDatabase("_system");
      try { db._dropDatabase(dbName); } catch (e) {}
      db._createDatabase(dbName);
      db._useDatabase(dbName);
      analyzers.save("MyTrigram", "ngram", { min: 2, max: 3, preserveOriginal: true });
      db._useDatabase("_system");
      try {
        analyzers.remove(dbName + "::MyTrigram");
        fail(); // removal with db name in wrong current used db should also fail
      } catch(e) {
        assertEqual(require("internal").errors.ERROR_FORBIDDEN .code,
                       e.errorNum);
      }
      db._useDatabase(dbName);
      analyzers.remove(dbName + "::MyTrigram", true);
      db._useDatabase("_system");
      db._dropDatabase(dbName);
    },
    testAnalyzerCreateRemovalWithDatabaseName_InDb: function() {
      let dbName = "analyzerWrongDbName2";
      db._useDatabase("_system");
      try { db._dropDatabase(dbName); } catch (e) {}
      db._createDatabase(dbName);
      try {
        analyzers.save(dbName + "::MyTrigram", "ngram", { min: 2, max: 3, preserveOriginal: true });
        fail();
      } catch(e) {
        assertEqual(require("internal").errors.ERROR_FORBIDDEN.code,
                       e.errorNum);
      }
      db._useDatabase(dbName);
      analyzers.save(dbName + "::MyTrigram", "ngram", { min: 2, max: 3, preserveOriginal: true }); 
      analyzers.remove(dbName + "::MyTrigram", true); 
      db._useDatabase("_system");
      db._dropDatabase(dbName);
    },
    testAnalyzerCreateRemovalWithDatabaseName_InSystem: function() {
      let dbName = "analyzerWrongDbName3";
      db._useDatabase("_system");
      try { db._dropDatabase(dbName); } catch (e) {}
      try { analyzers.remove("MyTrigram"); } catch (e) {}
      db._createDatabase(dbName);
      db._useDatabase(dbName);
      // cross-db request should fail
      try {
        analyzers.save("::MyTrigram", "ngram", { min: 2, max: 3, preserveOriginal: true });
        fail();
      } catch(e) {
        assertEqual(require("internal").errors.ERROR_FORBIDDEN.code,
                       e.errorNum);
      }
      try {
        analyzers.save("_system::MyTrigram", "ngram", { min: 2, max: 3, preserveOriginal: true });
        fail();
      } catch(e) {
        assertEqual(require("internal").errors.ERROR_FORBIDDEN.code,
                       e.errorNum);
      }
      // in _system db requests should be ok
      db._useDatabase("_system");
      analyzers.save("::MyTrigram", "ngram", { min: 2, max: 3, preserveOriginal: true }); 
      analyzers.remove("::MyTrigram", true); 
      analyzers.save("_system::MyTrigram", "ngram", { min: 2, max: 3, preserveOriginal: true }); 
      analyzers.remove("_system::MyTrigram", true); 
      db._dropDatabase(dbName);
    },
    testAnalyzerInvalidName: function() {
      let dbName = "analyzerWrongDbName4";
      db._useDatabase("_system");
      try { db._dropDatabase(dbName); } catch (e) {}
      db._createDatabase(dbName);
      db._useDatabase(dbName);
      try {
        // invalid ':' in name
        analyzers.save(dbName + "::MyTr:igram", "ngram", { min: 2, max: 3, preserveOriginal: true });
        fail();
      } catch(e) {
        assertEqual(require("internal").errors.ERROR_BAD_PARAMETER.code,
                       e.errorNum);
      }
      db._useDatabase("_system");
      db._dropDatabase(dbName);
    },
    testAnalyzerGetFromOtherDatabase: function() {
      let dbName = "analyzerDbName";
      let anotherDbName = "anotherDbName";
      db._useDatabase("_system");
      try { db._dropDatabase(dbName); } catch (e) {}
      try { db._dropDatabase(anotherDbName); } catch (e) {}
      db._createDatabase(dbName);
      db._createDatabase(anotherDbName);
      db._useDatabase(dbName);
      let analyzer = analyzers.save("MyTrigram", "ngram", { min: 2, max: 3, preserveOriginal: true });
      assertNotNull(analyzer);
      db._useDatabase(anotherDbName);
      try {
        analyzers.analyzer(dbName + "::MyTrigram");
        fail();
      } catch(e) {
        assertEqual(require("internal").errors.ERROR_FORBIDDEN .code,
                       e.errorNum);
      }
      db._useDatabase("_system");
      db._dropDatabase(dbName);
      db._dropDatabase(anotherDbName);
    },
    testAnalyzers: function() {
      let oldList = analyzers.toArray();
      let oldListInCollection = db._analyzers.toArray();
      assertTrue(Array === oldList.constructor);

      assertEqual(0, db._analyzers.count());

      // creation
      analyzers.save("testAnalyzer", "stem", { "locale":"en"}, [ "frequency" ]);

      // properties
      let analyzer = analyzers.analyzer(db._name() + "::testAnalyzer");
      assertTrue(null !== analyzer);
      assertEqual(db._name() + "::testAnalyzer", analyzer.name());
      assertEqual("stem", analyzer.type());
      assertEqual(1, Object.keys(analyzer.properties()).length);
      assertEqual("en", analyzer.properties().locale);
      assertTrue(Array === analyzer.features().constructor);
      assertEqual(1, analyzer.features().length);
      assertEqual([ "frequency" ], analyzer.features());

      // check persisted analyzer
      assertEqual(1, db._analyzers.count());
      let savedAnalyzer = db._analyzers.toArray()[0];
      assertTrue(null !== savedAnalyzer);
      assertEqual(7, Object.keys(savedAnalyzer).length);
      assertEqual("_analyzers/testAnalyzer", savedAnalyzer._id);
      assertEqual("testAnalyzer", savedAnalyzer._key);
      assertEqual("testAnalyzer", savedAnalyzer.name);
      assertEqual("stem", savedAnalyzer.type);
      assertEqual(analyzer.properties(), savedAnalyzer.properties);
      assertEqual(analyzer.features(), savedAnalyzer.features);

      analyzer = undefined; // release reference 

      // check the analyzers collection in database
      assertEqual(oldListInCollection.length + 1, db._analyzers.toArray().length);
      let dbAnalyzer = db._query("FOR d in _analyzers FILTER d.name=='testAnalyzer' RETURN d").toArray();
      assertEqual(1, dbAnalyzer.length);
      assertEqual("_analyzers/testAnalyzer", dbAnalyzer[0]._id);
      assertEqual("testAnalyzer", dbAnalyzer[0]._key);
      assertEqual("testAnalyzer", dbAnalyzer[0].name);
      assertEqual("stem", dbAnalyzer[0].type);
      assertEqual(1, Object.keys(dbAnalyzer[0].properties).length);
      assertEqual("en", dbAnalyzer[0].properties.locale);
      assertTrue(Array === dbAnalyzer[0].features.constructor);
      assertEqual(1, dbAnalyzer[0].features.length);
      assertEqual([ "frequency" ], dbAnalyzer[0].features);
      dbAnalyzer = undefined;

      // listing
      let list = analyzers.toArray();
      assertTrue(Array === list.constructor);
      assertEqual(oldList.length + 1, list.length);

      list = undefined; // release reference

      // force server-side V8 garbage collection
      if (db._connection !== undefined) { // client test
        let url = require('internal').arango.getEndpoint().replace('tcp', 'http');
        url += '/_admin/execute?returnAsJSON=true';
        let options = require('@arangodb/process-utils').makeAuthorizationHeaders({
          username: 'root',
          password: ''
        });
        options.method = 'POST';
        require('internal').download(url, 'require("internal").wait(0.1, true);', options);
      } else {
        require("internal").wait(0.1, true);
      }

      // removal
      analyzers.remove("testAnalyzer");
      assertTrue(null === analyzers.analyzer(db._name() + "::testAnalyzer"));
      assertEqual(oldList.length, analyzers.toArray().length);
      // check the analyzers collection in database
      assertEqual(oldListInCollection.length, db._analyzers.toArray().length);
    },

   testAnalyzersFeatures: function() {
      try {
       analyzers.save("testAnalyzer", "identity", {}, [ "unknown" ]);
       fail(); // unsupported feature
      } catch(e) {
      }

      try {
       analyzers.save("testAnalyzer", "identity", {}, [ "position" ]);
       fail(); // feature with dependency
      } catch(e) {
      }

      // feature with dependency satisfied
      analyzers.save("testAnalyzer", "identity", {}, [ "frequency", "position" ]);
      analyzers.remove("testAnalyzer", true);
    },

    testAnalyzersPrefix: function() {
      let dbName = "TestDB";
      db._useDatabase("_system");
      try { db._dropDatabase(dbName); } catch (e) {}
      db._createDatabase(dbName);
      db._useDatabase(dbName);

      let oldList = analyzers.toArray();
      assertTrue(Array === oldList.constructor);

      // creation
      db._useDatabase("_system");
      analyzers.save("testAnalyzer", "identity", {}, [ "frequency" ]);
      db._useDatabase(dbName);
      analyzers.save("testAnalyzer", "identity", {}, [ "norm" ]);

      // retrieval (dbName)
      db._useDatabase(dbName);

      {
        let analyzer = analyzers.analyzer("testAnalyzer");
        assertTrue(null !== analyzer);
        assertEqual(db._name() + "::testAnalyzer", analyzer.name());
        assertEqual("identity", analyzer.type());
        assertEqual(0, Object.keys(analyzer.properties()).length);
        assertTrue(Array === analyzer.features().constructor);
        assertEqual(1, analyzer.features().length);
        assertEqual([ "norm" ], analyzer.features());
      }

      {
        let analyzer = analyzers.analyzer("::testAnalyzer");
        assertTrue(null !== analyzer);
        assertEqual("_system::testAnalyzer", analyzer.name());
        assertEqual("identity", analyzer.type());
        assertEqual(0, Object.keys(analyzer.properties()).length);
        assertTrue(Array === analyzer.features().constructor);
        assertEqual(1, analyzer.features().length);
        assertEqual([ "frequency" ], analyzer.features());
      }

      // listing
      let list = analyzers.toArray();
      assertTrue(Array === list.constructor);
      assertEqual(oldList.length + 2, list.length);

      // removal
      analyzers.remove("testAnalyzer", true);
      assertTrue(null === analyzers.analyzer("testAnalyzer"));
      db._useDatabase("_system");
      analyzers.remove("testAnalyzer", true);
      db._useDatabase(dbName); // switch back to check analyzer with global name
      assertTrue(null === analyzers.analyzer("::testAnalyzer"));
      assertEqual(oldList.length, analyzers.toArray().length);

      db._useDatabase("_system");
      db._dropDatabase(dbName);
   },

////////////////////////////////////////////////////////////////////////////////
/// @brief IResearchFeature tests
////////////////////////////////////////////////////////////////////////////////

    testDefaultAnalyzers : function() {
      // invalid
      {
        try {
          db._query("RETURN TOKENS('a quick brown fox jumps', 'invalid')").toArray();
          fail();
        } catch (err) {
          assertEqual(err.errorNum, require("internal").errors.ERROR_BAD_PARAMETER.code);
        }
      }

      // text_de
      {
        let result = db._query(
          "RETURN TOKENS('a quick brown fox jumps', 'text_de')",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertTrue(Array === result[0].constructor);
        assertEqual(5, result[0].length);
        assertEqual([ "a", "quick", "brown", "fox", "jumps" ], result[0]);
      }

      // text_en
      {
        let result = db._query(
          "RETURN TOKENS('a quick brown fox jumps', 'text_en')",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertTrue(Array === result[0].constructor);
        assertEqual(5, result[0].length);
        assertEqual([ "a", "quick", "brown", "fox", "jump" ], result[0]);
      }

      // text_es
      {
        let result = db._query(
          "RETURN TOKENS('a quick brown fox jumps', 'text_es')",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertTrue(Array === result[0].constructor);
        assertEqual(5, result[0].length);
        assertEqual([ "a", "quick", "brown", "fox", "jumps" ], result[0]);
      }

      // text_fi
      {
        let result = db._query(
          "RETURN TOKENS('a quick brown fox jumps', 'text_fi')",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertTrue(Array === result[0].constructor);
        assertEqual(5, result[0].length);
        assertEqual([ "a", "quick", "brow", "fox", "jumps" ], result[0]);
      }

      // text_fr
      {
        let result = db._query(
          "RETURN TOKENS('a quick brown fox jumps', 'text_fr')",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertTrue(Array === result[0].constructor);
        assertEqual(5, result[0].length);
        assertEqual([ "a", "quick", "brown", "fox", "jump" ], result[0]);
      }

      // text_it
      {
        let result = db._query(
          "RETURN TOKENS('a quick brown fox jumps', 'text_it')",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertTrue(Array === result[0].constructor);
        assertEqual(5, result[0].length);
        assertEqual([ "a", "quick", "brown", "fox", "jumps" ], result[0]);
      }

      // text_nl
      {
        let result = db._query(
          "RETURN TOKENS('a quick brown fox jumps', 'text_nl')",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertTrue(Array === result[0].constructor);
        assertEqual(5, result[0].length);
        assertEqual([ "a", "quick", "brown", "fox", "jump" ], result[0]);
      }

      // text_no
      {
        let result = db._query(
          "RETURN TOKENS('a quick brown fox jumps', 'text_no')",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertTrue(Array === result[0].constructor);
        assertEqual(5, result[0].length);
        assertEqual([ "a", "quick", "brown", "fox", "jump" ], result[0]);
      }

      // text_pt
      {
        let result = db._query(
          "RETURN TOKENS('a quick brown fox jumps', 'text_pt')",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertTrue(Array === result[0].constructor);
        assertEqual(5, result[0].length);
        assertEqual([ "a", "quick", "brown", "fox", "jumps" ], result[0]);
      }

      // text_ru (codepoints)
      {
        let result = db._query(
          "RETURN TOKENS('ArangoDB - \u044D\u0442\u043E \u043C\u043D\u043E\u0433\u043E\u043C\u043E\u0434\u0435\u043B\u044C\u043D\u0430\u044F \u0431\u0430\u0437\u0430 \u0434\u0430\u043D\u043D\u044B\u0445', 'text_ru')",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertTrue(Array === result[0].constructor);
        assertEqual(5, result[0].length);
        assertEqual([ "arangodb", "\u044D\u0442", "\u043C\u043D\u043E\u0433\u043E\u043C\u043E\u0434\u0435\u043B\u044C\u043D", "\u0431\u0430\u0437", "\u0434\u0430\u043D" ], result[0]);
        assertEqual([ "arangodb", "эт", "многомодельн", "баз", "дан" ], result[0]);
      }

      // text_ru (unicode)
      {
        let result = db._query(
          "RETURN TOKENS('ArangoDB - это многомодельная база данных', 'text_ru')",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertTrue(Array === result[0].constructor);
        assertEqual(5, result[0].length);
        assertEqual([ "arangodb", "\u044D\u0442", "\u043C\u043D\u043E\u0433\u043E\u043C\u043E\u0434\u0435\u043B\u044C\u043D", "\u0431\u0430\u0437", "\u0434\u0430\u043D" ], result[0]);
        assertEqual([ "arangodb", "эт", "многомодельн", "баз", "дан" ], result[0]);
      }

      // text_sv
      {
        let result = db._query(
          "RETURN TOKENS('a quick brown fox jumps', 'text_sv')",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertTrue(Array === result[0].constructor);
        assertEqual(5, result[0].length);
        assertEqual([ "a", "quick", "brown", "fox", "jump" ], result[0]);
      }

      // text_zh (codepoints)
      {
        let result = db._query(
           "RETURN TOKENS('ArangoDB \u662F\u4E00\u4E2A\u591A\u6A21\u578B\u6570\u636E\u5E93\u3002', 'text_zh')",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertTrue(Array === result[0].constructor);
        assertEqual(7, result[0].length);
        assertEqual([ "arangodb", "\u662F", "\u4E00\u4E2A", "\u591A", "\u6A21\u578B", "\u6570\u636E", "\u5E93" ], result[0]);
        assertEqual([ "arangodb", "是", "一个", "多", "模型", "数据", "库" ], result[0]);
      }

      // text_zh (unicode)
      {
        let result = db._query(
          "RETURN TOKENS('ArangoDB 是一个多模型数据库。', 'text_zh')",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertTrue(Array === result[0].constructor);
        assertEqual(7, result[0].length);
        assertEqual([ "arangodb", "\u662F", "\u4E00\u4E2A", "\u591A", "\u6A21\u578B", "\u6570\u636E", "\u5E93" ], result[0]);
        assertEqual([ "arangodb", "是", "一个", "多", "模型", "数据", "库" ], result[0]);
      }

    },

    testNormAnalyzer : function() {
      let analyzerName = "normUnderTest";
      // case upper
      {
        analyzers.save(analyzerName, "norm", { "locale" : "en", "case": "upper" });
        let result = db._query(
          "RETURN TOKENS('fOx', '" + analyzerName + "' )",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertEqual(1, result[0].length);
        assertEqual([ "FOX" ], result[0]);
        analyzers.remove(analyzerName, true);
      }
      // case lower
      {
        analyzers.save(analyzerName, "norm",  {  "locale" : "en", "case": "lower" });
        let result = db._query(
          "RETURN TOKENS('fOx', '" + analyzerName + "' )",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertEqual(1, result[0].length);
        assertEqual([ "fox" ], result[0]);
        analyzers.remove(analyzerName, true);
      }
      // case none
      {
        analyzers.save(analyzerName, "norm", {  "locale" : "en", "case": "none" });
        let result = db._query(
          "RETURN TOKENS('fOx', '" + analyzerName + "' )",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertEqual(1, result[0].length);
        assertEqual([ "fOx" ], result[0]);
        analyzers.remove(analyzerName, true);
      }
       // accent removal
      {
        analyzers.save(analyzerName, "norm", {  "locale" : "de_DE.UTF8", "case": "none", "accent":false });
        let result = db._query(
          "RETURN TOKENS('\u00F6\u00F5', '" + analyzerName + "' )",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertEqual(1, result[0].length);
        assertEqual([ "\u006F\u006F" ], result[0]);
        analyzers.remove(analyzerName, true);
      }
       // accent leave
      {
        analyzers.save(analyzerName, "norm", {  "locale" : "de_DE.UTF8", "case": "none", "accent":true });
        let result = db._query(
          "RETURN TOKENS('\u00F6\u00F5', '" + analyzerName + "' )",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertEqual(1, result[0].length);
        assertEqual([ "\u00F6\u00F5" ], result[0]);
        analyzers.remove(analyzerName, true);
      }
      // no properties
      {
        let created = false;
        try {
          analyzers.save(analyzerName, "norm");
          analyzers.remove(analyzerName, true); // cleanup (should not get there)
          created = true;
        } catch (err) {
          assertEqual(err.errorNum, require("internal").errors.ERROR_BAD_PARAMETER.code);
        }
        assertFalse(created);
      }
    },
    testCustomStemAnalyzer : function() {
      let analyzerName = "stemUnderTest";
      {
        analyzers.save(analyzerName, "stem", {  "locale" : "en"});
        let result = db._query(
          "RETURN TOKENS('jumps', '" + analyzerName + "' )",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertEqual(1, result[0].length);
        assertEqual([ "jump" ], result[0]);
        analyzers.remove(analyzerName, true);
      }
      // no properties
      {
        try {
          analyzers.save(analyzerName, "stem");
          analyzers.remove(analyzerName, true); // cleanup (should not get there)
          fail();
        } catch (err) {
          assertEqual(err.errorNum, require("internal").errors.ERROR_BAD_PARAMETER.code);
        }
      }
    },
    testCustomTextAnalyzer : function() {
      let analyzerName = "textUnderTest";
      // case upper
      {
        analyzers.save(analyzerName, "text", { "locale" : "en", "case": "upper", "stopwords": [] });
        let result = db._query(
          "RETURN TOKENS('fOx', '" + analyzerName + "' )",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertEqual(1, result[0].length);
        assertEqual([ "FOX" ], result[0]);
        analyzers.remove(analyzerName, true);
      }
      // case lower
      {
        analyzers.save(analyzerName, "text", { "locale" : "en", "case": "lower", "stopwords": [] });
        let result = db._query(
          "RETURN TOKENS('fOx', '" + analyzerName + "' )",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertEqual(1, result[0].length);
        assertEqual([ "fox" ], result[0]);
        analyzers.remove(analyzerName, true);
      }
      // case none
      {
        analyzers.save(analyzerName, "text", {  "locale" : "en", "case": "none", "stopwords": [] });
        let result = db._query(
          "RETURN TOKENS('fOx', '" + analyzerName + "' )",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertEqual(1, result[0].length);
        assertEqual([ "fOx" ], result[0]);
        analyzers.remove(analyzerName, true);
      }
       // accent removal
      {
        analyzers.save(analyzerName, "text", {  "locale" : "de_DE.UTF8", "case": "none", "accent":false, "stopwords": [], "stemming":false });
        let result = db._query(
          "RETURN TOKENS('\u00F6\u00F5', '" + analyzerName + "' )",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertEqual(1, result[0].length);
        assertEqual([ "\u006F\u006F" ], result[0]);
        analyzers.remove(analyzerName, true);
      }
       // accent leave
      {
        analyzers.save(analyzerName, "text", {  "locale" : "de_DE.UTF8", "case": "none", "accent":true, "stopwords": [], "stemming":false});
        let result = db._query(
          "RETURN TOKENS('\u00F6\u00F5', '" + analyzerName + "' )",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertEqual(1, result[0].length);
        assertEqual([ "\u00F6\u00F5" ], result[0]);
        analyzers.remove(analyzerName, true);
      }

      // no stemming
      {
        analyzers.save(analyzerName, "text", {  "locale" : "en", "case": "none", "stemming":false, "stopwords": [] });
        let result = db._query(
          "RETURN TOKENS('jumps', '" + analyzerName + "' )",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertEqual(1, result[0].length);
        assertEqual([ "jumps" ], result[0]);
        analyzers.remove(analyzerName, true);
      }
      // stemming
      {
        analyzers.save(analyzerName, "text", {  "locale" : "en", "case": "none", "stemming":true, "stopwords": [] });
        let result = db._query(
          "RETURN TOKENS('jumps', '" + analyzerName + "' )",
          null,
          { }
        ).toArray();
        assertEqual(1, result.length);
        assertEqual(1, result[0].length);
        assertEqual([ "jump" ], result[0]);
        analyzers.remove(analyzerName, true);
      }
      // no properties
      {
        try {
          analyzers.save(analyzerName, "text");
          analyzers.remove(analyzerName, true); // cleanup (should not get there)
          fail();
        } catch (err) {
          assertEqual(err.errorNum, require("internal").errors.ERROR_BAD_PARAMETER.code);
        }
      }
    },
    testInvalidTypeAnalyzer : function() {
      let analyzerName = "unknownUnderTest";
      try {
          analyzers.save(analyzerName, "unknownAnalyzerType");
          analyzers.remove(analyzerName, true); // cleanup (should not get there)
          fail();
      } catch (err) {
          assertEqual(err.errorNum, require("internal").errors.ERROR_NOT_IMPLEMENTED.code);
      }
    }
  };
}

////////////////////////////////////////////////////////////////////////////////
/// @brief executes the test suite
////////////////////////////////////////////////////////////////////////////////

jsunity.run(iResearchFeatureAqlTestSuite);

return jsunity.done();
