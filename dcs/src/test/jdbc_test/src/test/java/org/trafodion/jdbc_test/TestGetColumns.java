
// @@@ START COPYRIGHT @@@
//
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership. The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the License for the
// specific language governing permissions and limitations
// under the License.
//
// @@@ END COPYRIGHT @@@

import java.sql.Connection;
import java.sql.DatabaseMetaData;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.Statement;
import java.sql.Types;

import org.junit.AfterClass;
import org.junit.BeforeClass;
import org.junit.Test;
import static org.junit.Assert.*;

public class TestGetColumns {
  private static final String CATALOG = "TRAFODION";
  private static final String SCHEMA = "SEABASE";

  private static final String COLUMNS_TEST_TABLE_1 = "COLUMNS_TEST_TABLE_1";
  private static final String strCreateTableQuery_1 =
      "CREATE TABLE " + CATALOG + "." + SCHEMA + "." + COLUMNS_TEST_TABLE_1
          + "( id LARGEINT GENERATED ALWAYS AS IDENTITY, description VARCHAR(40) )";
  private static final String strDropTableQuery_1 =
      "DROP TABLE " + CATALOG + "." + SCHEMA + "." + COLUMNS_TEST_TABLE_1;

  private static final String COLUMNS_TEST_TABLE_2 = "COLUMNS_TEST_TABLE_2";
  private static final String strCreateTableQuery_2 =
      "CREATE TABLE " + CATALOG + "." + SCHEMA + "." + COLUMNS_TEST_TABLE_2
          + "( id LARGEINT GENERATED BY DEFAULT AS IDENTITY, description VARCHAR(40) )";
  private static final String strDropTableQuery_2 =
      "DROP TABLE " + CATALOG + "." + SCHEMA + "." + COLUMNS_TEST_TABLE_2;

  private static Connection _conn = null;

  @BeforeClass
  public static void doTestSuiteSetup() throws Exception {
    try {
      _conn = Utils.getUserConnection();
    } catch (Exception e) {
      fail("failed to create connection" + e.getMessage());
    }

    try (Statement stmt = _conn.createStatement()) {
      stmt.execute(strCreateTableQuery_1);
      stmt.execute(strCreateTableQuery_2);
    } catch (Exception e) {
      fail("failed to create the table : " + e.getMessage());
    }
  }

  @Test
  public void testGetColumns() {
    Columns[] expColumns_1 = {
        new Columns(CATALOG, SCHEMA, COLUMNS_TEST_TABLE_1, "ID", -5, "BIGINT", 19, 20, 0, 10, 0, "",
            "", -402, 0, 0, 1, "NO", null, null, null, (short) 0, "YES", "NO"),
        new Columns(CATALOG, SCHEMA, COLUMNS_TEST_TABLE_1, "DESCRIPTION", 12, "VARCHAR", 40, 40, 0,
            0, 1, "", "", 12, 0, 40, 2, "YES", null, null, null, (short) 0, "NO", "NO") };
    Columns[] expColumns_2 = {
        new Columns(CATALOG, SCHEMA, COLUMNS_TEST_TABLE_2, "ID", -5, "BIGINT", 19, 20, 0, 10, 0, "",
            "", -402, 0, 0, 1, "NO", null, null, null, (short) 0, "NO", "YES"),
        new Columns(CATALOG, SCHEMA, COLUMNS_TEST_TABLE_2, "DESCRIPTION", 12, "VARCHAR", 40, 40, 0,
            0, 1, "", "", 12, 0, 40, 2, "YES", null, null, null, (short) 0, "NO", "NO") };
    try {
      DatabaseMetaData meta = _conn.getMetaData();
      ResultSet columnsRS_1 = meta.getColumns(CATALOG, SCHEMA, COLUMNS_TEST_TABLE_1, "%");
      int rowNum = 0;
      while (columnsRS_1.next()) {
        compareColumnsWithExp("testGetColumns", rowNum + 1, columnsRS_1, expColumns_1[rowNum]);
        rowNum++;
      }
      assertEquals(rowNum, 2);
      ResultSet columnsRS_2 = meta.getColumns(CATALOG, SCHEMA, COLUMNS_TEST_TABLE_2, "%");
      rowNum = 0;
      while (columnsRS_2.next()) {
        compareColumnsWithExp("testGetColumns", rowNum + 1, columnsRS_2, expColumns_2[rowNum]);
        rowNum++;
      }
      assertEquals(rowNum, 2);
    } catch (Exception e) {
      System.out.println(e.getMessage());
      e.printStackTrace();
    }
  }

  @AfterClass
  public static void cleanTable() {
    try (Statement stmt = _conn.createStatement()) {
      stmt.execute(strDropTableQuery_1);
      stmt.execute(strDropTableQuery_2);
    } catch (Exception e) {
    }

    try {
      _conn.close();
    } catch (Exception e) {

    }
  }

  private class Columns {
    public String tableCat = "TRAFODION";
    public String tableSchem = "SCH";
    public String tableName;
    public String columnName;
    public int dataType;
    public String typeName;
    public int columnSize;
    public int bufferLength;
    public int decimalDigits;
    public int numPrecRadix;
    public int nullable;
    public String remarks;
    public String columnDef;
    public int sqlDataType;
    public int sqlDatetimeSub;
    public int charOctetLength;
    public int ordinalPosition;
    public String isNullable;
    public String scopeCatalog;
    public String scopeSchema;
    public String scopeTable;
    public short sourceDataType;
    public String isAutoincrement;
    public String isGeneratedcolumn;

    public Columns(String tableCat, String tableSchem, String tableName, String columnName,
        int dataType, String typeName, int columnSize, int bufferLength, int decimalDigits,
        int numPrecRadix, int nullable, String remarks, String columnDef, int sqlDataType,
        int sqlDatetimeSub, int charOctetLength, int ordinalPosition, String isNullable,
        String scopeCatalog, String scopeSchema, String scopeTable, short sourceDataType,
        String isAutoincrement, String isGeneratedcolumn) {

      super();
      this.tableCat = tableCat;
      this.tableSchem = tableSchem;
      this.tableName = tableName;
      this.columnName = columnName;
      this.dataType = dataType;
      this.typeName = typeName;
      this.columnSize = columnSize;
      this.bufferLength = bufferLength;
      this.decimalDigits = decimalDigits;
      this.numPrecRadix = numPrecRadix;
      this.nullable = nullable;
      this.remarks = remarks;
      this.columnDef = columnDef;
      this.sqlDataType = sqlDataType;
      this.sqlDatetimeSub = sqlDatetimeSub;
      this.charOctetLength = charOctetLength;
      this.ordinalPosition = ordinalPosition;
      this.isNullable = isNullable;
      this.scopeCatalog = scopeCatalog;
      this.scopeSchema = scopeSchema;
      this.scopeTable = scopeTable;
      this.sourceDataType = sourceDataType;
      this.isAutoincrement = isAutoincrement;
      this.isGeneratedcolumn = isGeneratedcolumn;
    }
  }

  private void compareColumnsWithExp(String methondName, int rowNum, ResultSet rs,
      Columns columns) {
    try {
      assertEquals(methondName + " rowNum " + Integer.toString(rowNum) + " tableCat ",
        columns.tableCat, rs.getString("TABLE_CAT"));
      assertEquals(methondName + " rowNum " + Integer.toString(rowNum) + " tableSchem ",
        columns.tableSchem, rs.getString("TABLE_SCHEM"));
      assertEquals(methondName + " rowNum " + Integer.toString(rowNum) + " tableName ",
        columns.tableName, rs.getString("TABLE_NAME"));
      assertEquals(methondName + " rowNum " + Integer.toString(rowNum) + " columnName ",
        columns.columnName, rs.getString("COLUMN_NAME"));
      assertEquals(methondName + " rowNum " + Integer.toString(rowNum) + " dataType ",
        columns.dataType, rs.getInt("DATA_TYPE"));
      assertEquals(methondName + " rowNum " + Integer.toString(rowNum) + " typeName ",
        columns.typeName, rs.getString("TYPE_NAME"));
      assertEquals(methondName + " rowNum " + Integer.toString(rowNum) + " columnSize ",
        columns.columnSize, rs.getInt("COLUMN_SIZE"));
      assertEquals(methondName + " rowNum " + Integer.toString(rowNum) + " bufferLength ",
        columns.bufferLength, rs.getInt("BUFFER_LENGTH"));
      assertEquals(methondName + " rowNum " + Integer.toString(rowNum) + " decimalDigits ",
        columns.decimalDigits, rs.getInt("DECIMAL_DIGITS"));
      assertEquals(methondName + " rowNum " + Integer.toString(rowNum) + " numPrecRadix ",
        columns.numPrecRadix, rs.getInt("NUM_PREC_RADIX"));
      assertEquals(methondName + " rowNum " + Integer.toString(rowNum) + " nullable ",
        columns.nullable, rs.getInt("NULLABLE"));
      assertEquals(methondName + " rowNum " + Integer.toString(rowNum) + " remarks ",
        columns.remarks, rs.getString("REMARKS"));
      assertEquals(methondName + " rowNum " + Integer.toString(rowNum) + " columnDef ",
        columns.columnDef, rs.getString("COLUMN_DEF"));
      assertEquals(methondName + " rowNum " + Integer.toString(rowNum) + " sqlDataType ",
        columns.sqlDataType, rs.getInt("SQL_DATA_TYPE"));
      assertEquals(methondName + " rowNum " + Integer.toString(rowNum) + " sqlDatetimeSub ",
        columns.sqlDatetimeSub, rs.getInt("SQL_DATETIME_SUB"));
      assertEquals(methondName + " rowNum " + Integer.toString(rowNum) + " charOctetLength ",
        columns.charOctetLength, rs.getInt("CHAR_OCTET_LENGTH"));
      assertEquals(methondName + " rowNum " + Integer.toString(rowNum) + " ordinalPosition ",
        columns.ordinalPosition, rs.getInt("ORDINAL_POSITION"));
      assertEquals(methondName + " rowNum " + Integer.toString(rowNum) + " isNullable ",
        columns.isNullable, rs.getString("IS_NULLABLE"));

      String scopeCatalog = rs.getString("SCOPE_CATALOG");
      if (columns.dataType != Types.REF) {
        assertTrue(rs.wasNull());
      } else {
        assertEquals(methondName + " rowNum " + Integer.toString(rowNum) + " scopeCatalog ",
          columns.scopeCatalog, scopeCatalog);
      }
      String scopeSchema = rs.getString("SCOPE_SCHEMA");
      if (columns.dataType != Types.REF) {
        assertTrue(rs.wasNull());
      } else {
        assertEquals(methondName + " rowNum " + Integer.toString(rowNum) + " scopeSchema ",
          columns.scopeSchema, scopeSchema);
      }
      String scopeTable = rs.getString("SCOPE_TABLE");
      if (columns.dataType != Types.REF) {
        assertTrue(rs.wasNull());
      } else {
        assertEquals(methondName + " rowNum " + Integer.toString(rowNum) + " scopeTable ",
          columns.scopeTable, scopeTable);
      }
      short sourceDataType = rs.getShort("SOURCE_DATA_TYPE");
      if (columns.dataType != Types.REF) {
        assertTrue(rs.wasNull());
      } else {
        assertEquals(methondName + " rowNum " + Integer.toString(rowNum) + " sourceDataType ",
          columns.sourceDataType, sourceDataType);
      }
      assertEquals(methondName + " rowNum " + Integer.toString(rowNum) + " isAutoincrement ",
        columns.isAutoincrement, rs.getString("IS_AUTOINCREMENT"));
      assertEquals(methondName + " rowNum " + Integer.toString(rowNum) + " isGeneratedcolumn ",
        columns.isGeneratedcolumn, rs.getString("IS_GENERATEDCOLUMN"));

    } catch (Exception e) {
      System.out.println(e.getMessage());
      e.printStackTrace();
    }
  }
}
