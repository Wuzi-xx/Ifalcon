%% ============================================================
% 文件名: export_stereo_params_to_cpp.m
% 功能: 
%   从 stereoParams.mat 文件中读取双目标定结果，
%   自动提取左右相机的内参矩阵（K1, K2）和外参（R, T），
%   并以 C++ (OpenCV) 格式打印输出，方便嵌入到 C++ 代码中。
%
% 适用场景:
%   - MATLAB 进行相机标定后，需将结果移植至 C++ 工程使用。
%   - 输出格式直接可用于 OpenCV：
%       cv::Matx33d R(...);
%       cv::Vec3d  T(...);
%
% 作者: liu
% 日期: 2025-10-27
% 环境: MATLAB R2023b / R2024a 及以上版本
% ============================================================

function get_mat()
    %% === 1. 设置文件路径 ===
    % 根目录为你的项目文件夹（可根据实际修改）
    base_path = 'D:\Desktop\Proj\CameraDistance';       % 根目录路径
    mat_path = fullfile(base_path, 'stereoParams.mat'); % 拼接得到完整路径

    %% === 2. 检查文件是否存在 ===
    if exist(mat_path, 'file')
        fprintf('📂 正在处理: %s\n', mat_path);

        %% === 3. 读取 stereoParams.mat 文件 ===
        % 该文件通常由 MATLAB “Stereo Camera Calibrator” App 生成，
        % 内部包含 CameraParameters1, CameraParameters2, PoseCamera2 等结构。
        data = load(mat_path);
        stereoParams = data.stereoParams;

        %% === 4. 提取相机内参 ===
        % 内参矩阵为 3x3，OpenCV 习惯使用转置矩阵。
        % 例如: K = [fx  0  cx;
        %             0  fy  cy;
        %             0   0   1];
        K1 = stereoParams.CameraParameters1.IntrinsicMatrix';  % 左相机内参矩阵
        K2 = stereoParams.CameraParameters2.IntrinsicMatrix';  % 右相机内参矩阵

        %% === 5. 提取外参（R、T） ===
        % PoseCamera2 代表右相机相对于左相机的外参：
        % R 为旋转矩阵，T 为平移向量。
        R = stereoParams.PoseCamera2.Rotation;          % 旋转矩阵 (3x3)
        T = stereoParams.PoseCamera2.Translation;       % 平移向量 (1x3)

        %% === 6. 提取 fx, fy, cx, cy 四个关键内参参数 ===
        fx1 = K1(1,1); fy1 = K1(2,2); cx1 = K1(1,3); cy1 = K1(2,3);
        fx2 = K2(1,1); fy2 = K2(2,2); cx2 = K2(1,3); cy2 = K2(2,3);

        %% === 7. 输出为 C++ 变量定义格式 ===
        % 输出的格式可直接复制到 C++ 源代码中使用。
        fprintf('\n===== 🎯 导出为 C++ 格式 =====\n\n');
        
        % 左右相机内参
        fprintf('double fx1 = %.4f, fy1 = %.4f, cx1 = %.4f, cy1 = %.4f;\n', ...
                fx1, fy1, cx1, cy1);
        fprintf('double fx2 = %.4f, fy2 = %.4f, cx2 = %.4f, cy2 = %.4f;\n\n', ...
                fx2, fy2, cx2, cy2);

        % 旋转矩阵 R
        fprintf('cv::Matx33d R(\n');
        fprintf('    %.6f, %.6f, %.6f,\n', R(1,1), R(1,2), R(1,3));
        fprintf('    %.6f, %.6f, %.6f,\n', R(2,1), R(2,2), R(2,3));
        fprintf('    %.6f, %.6f, %.6f\n', R(3,1), R(3,2), R(3,3));
        fprintf(');\n');

        % 平移向量 T
        fprintf('cv::Vec3d T(%.6f, %.6f, %.6f);\n\n', T(1), T(2), T(3));

        fprintf('✅ 导出完成，请复制以上内容至 C++ 工程。\n');

    else
        %% === 文件未找到处理 ===
        fprintf('⚠️ 未找到文件: %s\n', mat_path);
        fprintf('请确认路径或文件名是否正确。\n');
    end
end
