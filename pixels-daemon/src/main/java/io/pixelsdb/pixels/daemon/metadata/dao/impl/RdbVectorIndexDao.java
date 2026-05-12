/*
 * Copyright 2026 PixelsDB.
 */
package io.pixelsdb.pixels.daemon.metadata.dao.impl;

import io.pixelsdb.pixels.common.utils.MetaDBUtil;
import io.pixelsdb.pixels.daemon.MetadataProto;
import io.pixelsdb.pixels.daemon.metadata.dao.VectorIndexDao;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.List;

public class RdbVectorIndexDao extends VectorIndexDao
{
    private static final Logger log = LogManager.getLogger(RdbVectorIndexDao.class);
    private static final MetaDBUtil db = MetaDBUtil.Instance();

    @Override
    public MetadataProto.VectorIndex getById(long id)
    {
        Connection conn = db.getConnection();
        try (Statement st = conn.createStatement())
        {
            ResultSet rs = st.executeQuery("SELECT * FROM VECTOR_INDICES WHERE VI_ID=" + id);
            if (rs.next())
            {
                return toProto(rs);
            }
        }
        catch (SQLException e)
        {
            log.error("getById in RdbVectorIndexDao", e);
        }
        return null;
    }

    @Override
    public List<MetadataProto.VectorIndex> getAllByTableId(long tableId)
    {
        Connection conn = db.getConnection();
        try (Statement st = conn.createStatement())
        {
            ResultSet rs = st.executeQuery("SELECT * FROM VECTOR_INDICES WHERE TBLS_TBL_ID=" + tableId);
            List<MetadataProto.VectorIndex> vectorIndices = new ArrayList<>();
            while (rs.next())
            {
                vectorIndices.add(toProto(rs));
            }
            return vectorIndices;
        }
        catch (SQLException e)
        {
            log.error("getAllByTableId in RdbVectorIndexDao", e);
        }
        return null;
    }

    @Override
    public boolean exists(MetadataProto.VectorIndex vectorIndex)
    {
        Connection conn = db.getConnection();
        try (Statement st = conn.createStatement())
        {
            ResultSet rs = st.executeQuery("SELECT 1 FROM VECTOR_INDICES WHERE VI_ID=" + vectorIndex.getId());
            return rs.next();
        }
        catch (SQLException e)
        {
            log.error("exists in RdbVectorIndexDao", e);
        }
        return false;
    }

    @Override
    public long insert(MetadataProto.VectorIndex vectorIndex)
    {
        Connection conn = db.getConnection();
        String sql = "INSERT INTO VECTOR_INDICES(`VI_VECTOR_COLUMN_ID`,`VI_METRIC`,`VI_DIMENSION`,`VI_INDEX_SCHEME`," +
                "`VI_PARAMS_JSON`,`TBLS_TBL_ID`,`SCHEMA_VERSIONS_SV_ID`) VALUES (?,?,?,?,?,?,?)";
        try (PreparedStatement pst = conn.prepareStatement(sql))
        {
            pst.setLong(1, vectorIndex.getVectorColumnId());
            pst.setString(2, vectorIndex.getMetric());
            pst.setInt(3, vectorIndex.getDimension());
            pst.setString(4, vectorIndex.getIndexScheme());
            pst.setString(5, vectorIndex.getParamsJson());
            pst.setLong(6, vectorIndex.getTableId());
            pst.setLong(7, vectorIndex.getSchemaVersionId());
            if (pst.executeUpdate() == 1)
            {
                ResultSet rs = pst.executeQuery("SELECT LAST_INSERT_ID()");
                if (rs.next())
                {
                    return rs.getLong(1);
                }
            }
        }
        catch (SQLException e)
        {
            log.error("insert in RdbVectorIndexDao", e);
        }
        return -1;
    }

    @Override
    public boolean update(MetadataProto.VectorIndex vectorIndex)
    {
        Connection conn = db.getConnection();
        String sql = "UPDATE VECTOR_INDICES SET `VI_VECTOR_COLUMN_ID`=?,`VI_METRIC`=?,`VI_DIMENSION`=?,`VI_INDEX_SCHEME`=?," +
                "`VI_PARAMS_JSON`=?,`TBLS_TBL_ID`=?,`SCHEMA_VERSIONS_SV_ID`=? WHERE `VI_ID`=?";
        try (PreparedStatement pst = conn.prepareStatement(sql))
        {
            pst.setLong(1, vectorIndex.getVectorColumnId());
            pst.setString(2, vectorIndex.getMetric());
            pst.setInt(3, vectorIndex.getDimension());
            pst.setString(4, vectorIndex.getIndexScheme());
            pst.setString(5, vectorIndex.getParamsJson());
            pst.setLong(6, vectorIndex.getTableId());
            pst.setLong(7, vectorIndex.getSchemaVersionId());
            pst.setLong(8, vectorIndex.getId());
            return pst.executeUpdate() == 1;
        }
        catch (SQLException e)
        {
            log.error("update in RdbVectorIndexDao", e);
        }
        return false;
    }

    @Override
    public boolean deleteById(long id)
    {
        Connection conn = db.getConnection();
        try (PreparedStatement pst = conn.prepareStatement("DELETE FROM VECTOR_INDICES WHERE VI_ID=?"))
        {
            pst.setLong(1, id);
            return pst.executeUpdate() == 1;
        }
        catch (SQLException e)
        {
            log.error("deleteById in RdbVectorIndexDao", e);
        }
        return false;
    }

    private MetadataProto.VectorIndex toProto(ResultSet rs) throws SQLException
    {
        return MetadataProto.VectorIndex.newBuilder()
                .setId(rs.getLong("VI_ID"))
                .setVectorColumnId(rs.getLong("VI_VECTOR_COLUMN_ID"))
                .setMetric(rs.getString("VI_METRIC"))
                .setDimension(rs.getInt("VI_DIMENSION"))
                .setIndexScheme(rs.getString("VI_INDEX_SCHEME"))
                .setParamsJson(rs.getString("VI_PARAMS_JSON"))
                .setTableId(rs.getLong("TBLS_TBL_ID"))
                .setSchemaVersionId(rs.getLong("SCHEMA_VERSIONS_SV_ID"))
                .build();
    }
}
